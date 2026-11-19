#include "runtime.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ailinux::ai {
namespace {

std::string task_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm value{};
    gmtime_r(&time, &value);
    std::array<char, 32> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &value);
    return buffer.data();
}

} // namespace

AgentScheduler::AgentScheduler(AgentReputationStore& reputation,
                               const ProviderPool& providers,
                               std::size_t max_parallel_tasks,
                               std::size_t max_tasks_per_agent,
                               std::size_t max_outstanding_tasks)
    : reputation_(reputation), providers_(providers), lease_(reputation),
      max_tasks_per_agent_(max_tasks_per_agent),
      max_outstanding_tasks_(max_outstanding_tasks) {
    if (max_parallel_tasks == 0 || max_tasks_per_agent_ == 0 ||
        max_outstanding_tasks_ == 0) {
        throw std::invalid_argument("scheduler task limits must be positive");
    }
    static_cast<void>(reputation_.recover_incomplete_tasks(task_timestamp()));
    workers_.reserve(max_parallel_tasks);
    for (std::size_t index = 0; index < max_parallel_tasks; ++index) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

AgentScheduler::~AgentScheduler() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        for (auto& [_, job] : jobs_) {
            job->cancelled.store(true);
        }
    }
    condition_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

std::future<AgentTaskResult> AgentScheduler::submit(AgentTask task) {
    if (task.task_id.empty() || task.agent_id.empty()) {
        throw std::invalid_argument("task and Agent IDs must not be empty");
    }
    if (task.priority < -1000 || task.priority > 1000) {
        throw std::invalid_argument("task priority must be between -1000 and 1000");
    }
    if (task.timeout.has_value() &&
        (task.timeout->count() <= 0 ||
         *task.timeout > std::chrono::hours(24))) {
        throw std::invalid_argument(
            "task timeout must be greater than zero and at most 24 hours");
    }
    const auto identity = reputation_.get_agent(task.agent_id);
    if (identity.status != "ready") {
        throw EligibilityError("cannot schedule work for an Agent that is not ready");
    }
    task.request.model = identity.model;
    static_cast<void>(task.request.to_json());
    auto job = std::make_shared<Job>();
    job->task = std::move(task);
    if (job->task.timeout.has_value()) {
        job->deadline =
            std::chrono::steady_clock::now() + *job->task.timeout;
    }
    auto future = job->promise.get_future();
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            throw ReputationError("scheduler is stopping");
        }
        if (jobs_.size() >= max_outstanding_tasks_) {
            throw SchedulerCapacityError("scheduler outstanding-task limit reached");
        }
        if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            throw ReputationError("scheduler submission sequence is exhausted");
        }
        per_agent_task_limits_[job->task.agent_id] =
            std::min(max_tasks_per_agent_, identity.max_concurrent_tasks);
        const auto record = reputation_.record_task_submission(
            job->task.task_id, job->task.agent_id, job->task.domain,
            job->task.difficulty, task_timestamp());
        if (record.state != "queued") {
            throw ReputationError("new task did not enter the queued state");
        }
        const auto [_, inserted] = jobs_.emplace(job->task.task_id, job);
        if (!inserted) {
            throw TaskConflictError("task is already present in the scheduler");
        }
        job->sequence = next_sequence_++;
        queue_.push(job);
    }
    condition_.notify_all();
    return future;
}

TaskRecord AgentScheduler::status(const std::string& task_id) const {
    return reputation_.get_task_record(task_id);
}

bool AgentScheduler::cancel(const std::string& task_id) {
    std::shared_ptr<Job> job;
    {
        std::lock_guard lock(mutex_);
        const auto found = jobs_.find(task_id);
        if (found != jobs_.end()) {
            job = found->second;
        }
    }
    if (!job) {
        const auto record = reputation_.get_task_record(task_id);
        return record.state == "cancelled";
    }
    std::lock_guard lock(job->lifecycle_mutex);
    if (job->terminal) {
        return false;
    }
    job->cancelled.store(true);
    condition_.notify_all();
    return true;
}

void AgentScheduler::worker_loop() {
    while (true) {
        std::shared_ptr<Job> selected;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] {
                if (stopping_ && queue_.empty()) {
                    return true;
                }
                auto pending = queue_;
                while (!pending.empty()) {
                    const auto& job = pending.top();
                    if (job->cancelled.load() ||
                        active_per_agent_[job->task.agent_id] <
                            per_agent_task_limits_.at(job->task.agent_id)) {
                        return true;
                    }
                    pending.pop();
                }
                return false;
            });
            if (stopping_ && queue_.empty()) {
                return;
            }
            decltype(queue_) deferred;
            while (!queue_.empty()) {
                auto candidate = queue_.top();
                queue_.pop();
                if (!selected &&
                    (candidate->cancelled.load() ||
                     active_per_agent_[candidate->task.agent_id] <
                         per_agent_task_limits_.at(candidate->task.agent_id))) {
                    selected = std::move(candidate);
                    if (!selected->cancelled.load()) {
                        ++active_per_agent_[selected->task.agent_id];
                    }
                } else {
                    deferred.push(std::move(candidate));
                }
            }
            queue_.swap(deferred);
        }
        if (selected) {
            execute(selected);
            {
                std::lock_guard lock(mutex_);
                const auto active = active_per_agent_.find(selected->task.agent_id);
                if (active != active_per_agent_.end() && active->second > 0) {
                    --active->second;
                }
                jobs_.erase(selected->task.task_id);
            }
            condition_.notify_all();
        }
    }
}

void AgentScheduler::execute(const std::shared_ptr<Job>& job) {
    bool cost_reserved = false;
    double reserved_cost_usd = 0.0;
    const auto finish = [this, &job, &cost_reserved,
                         &reserved_cost_usd](std::string state,
                                             std::optional<std::string> category,
                                             std::optional<ChatResponse> response = std::nullopt,
                                             bool settle_reserved_cost = false) {
        std::lock_guard lock(job->lifecycle_mutex);
        const auto cost_response = response;
        if (job->cancelled.load()) {
            state = "cancelled";
            category.reset();
            response.reset();
        }
        TaskRecord record;
        try {
            if (cost_reserved) {
                if (cost_response.has_value()) {
                    reputation_.settle_task_cost(
                        job->task.task_id,
                        providers_.actual_cost_usd(
                            reputation_.get_agent(job->task.agent_id).provider,
                            job->task.request, *cost_response));
                } else if (settle_reserved_cost) {
                    reputation_.settle_task_cost(job->task.task_id,
                                                 reserved_cost_usd);
                } else {
                    reputation_.release_task_cost(job->task.task_id);
                }
                cost_reserved = false;
            }
            const auto prompt_tokens =
                cost_response.has_value() ? cost_response->prompt_tokens : std::nullopt;
            const auto completion_tokens =
                cost_response.has_value() ? cost_response->completion_tokens
                                          : std::nullopt;
            record = reputation_.transition_task(job->task.task_id, state,
                                                 task_timestamp(), category,
                                                 prompt_tokens, completion_tokens);
        } catch (...) {
            job->terminal = true;
            job->promise.set_exception(std::current_exception());
            return;
        }
        job->terminal = true;
        job->promise.set_value({std::move(record), std::move(response)});
    };
    try {
        if (job->cancelled.load()) {
            finish("cancelled", std::nullopt);
            return;
        }
        if (job->deadline.has_value() &&
            std::chrono::steady_clock::now() >= *job->deadline) {
            finish("failed", "task_timeout");
            return;
        }
        const auto identity = reputation_.get_agent(job->task.agent_id);
        if (identity.status != "ready") {
            finish("failed", "agent_not_ready");
            return;
        }
        if (reputation_.cost_budget(job->task.agent_id).has_value()) {
            reserved_cost_usd =
                providers_.maximum_cost_usd(identity.provider, job->task.request);
            if (!reputation_.reserve_task_cost(job->task.task_id, job->task.agent_id,
                                               reserved_cost_usd)) {
                throw ReputationError(
                    "configured Agent cost budget could not reserve task cost");
            }
            cost_reserved = true;
        }
        static_cast<void>(reputation_.transition_task(
            job->task.task_id, "running", task_timestamp()));
        if (!reputation_.reserve_provider_request(job->task.task_id,
                                                 job->task.agent_id)) {
            finish("failed", "request_limit_exceeded");
            return;
        }
        const auto response = providers_.complete(identity.provider, job->task.request,
                                                   &job->cancelled, job->deadline);
        finish("completed", std::nullopt, response);
    } catch (const ProviderCancelledError& error) {
        finish("cancelled", std::nullopt, std::nullopt,
               error.request_started);
    } catch (const ProviderTaskTimeoutError& error) {
        finish("failed", "task_timeout", std::nullopt, error.request_started);
    } catch (const ProviderError& error) {
        finish("failed", error.category, std::nullopt, true);
    } catch (const CostBudgetExceededError&) {
        finish("failed", "cost_budget_exceeded");
    } catch (const ProviderConfigurationError&) {
        finish("failed", "ProviderConfigurationError");
    } catch (const ProviderProtocolError&) {
        finish("failed", "ProviderProtocolError", std::nullopt, true);
    } catch (const std::invalid_argument&) {
        finish("failed", "ValueError");
    } catch (const std::exception& error) {
        const std::string category =
            dynamic_cast<const EligibilityError*>(&error) != nullptr
                ? "agent_not_ready"
                : "request_failed";
        finish("failed", category, std::nullopt, true);
    }
}

} // namespace ailinux::ai
