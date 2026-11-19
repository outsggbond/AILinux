#pragma once

#include "providers.hpp"
#include "reputation.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ailinux::ai {

enum class AgentTaskState { queued, running, completed, failed, cancelled, interrupted };

class SchedulerCapacityError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct AgentTask {
    std::string task_id;
    std::string agent_id;
    std::string domain;
    int difficulty = 1;
    ChatRequest request;
    int priority = 0;
    std::optional<std::chrono::milliseconds> timeout = std::nullopt;
};

struct AgentTaskResult {
    TaskRecord status;
    std::optional<ChatResponse> response;
};

class AgentScheduler {
public:
    AgentScheduler(AgentReputationStore& reputation, const ProviderPool& providers,
                   std::size_t max_parallel_tasks = 8,
                   std::size_t max_tasks_per_agent = 2,
                   std::size_t max_outstanding_tasks = 256);
    ~AgentScheduler();
    AgentScheduler(const AgentScheduler&) = delete;
    AgentScheduler& operator=(const AgentScheduler&) = delete;

    [[nodiscard]] std::future<AgentTaskResult> submit(AgentTask task);
    [[nodiscard]] TaskRecord status(const std::string& task_id) const;
    [[nodiscard]] bool cancel(const std::string& task_id);

private:
    struct Job {
        AgentTask task;
        std::uint64_t sequence = 0;
        std::optional<std::chrono::steady_clock::time_point> deadline;
        std::promise<AgentTaskResult> promise;
        std::atomic<bool> cancelled{false};
        std::mutex lifecycle_mutex;
        bool terminal = false;
    };

    struct JobPriority {
        bool operator()(const std::shared_ptr<Job>& left,
                        const std::shared_ptr<Job>& right) const noexcept {
            if (left->task.priority != right->task.priority) {
                return left->task.priority < right->task.priority;
            }
            return left->sequence > right->sequence;
        }
    };

    void worker_loop();
    void execute(const std::shared_ptr<Job>& job);

    AgentReputationStore& reputation_;
    const ProviderPool& providers_;
    SchedulerLease lease_;
    std::size_t max_tasks_per_agent_;
    std::size_t max_outstanding_tasks_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::priority_queue<std::shared_ptr<Job>,
                        std::vector<std::shared_ptr<Job>>, JobPriority> queue_;
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs_;
    std::unordered_map<std::string, std::size_t> active_per_agent_;
    std::unordered_map<std::string, std::size_t> per_agent_task_limits_;
    std::vector<std::thread> workers_;
    std::uint64_t next_sequence_ = 0;
    bool stopping_ = false;
};

} // namespace ailinux::ai
