#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ailinux::ai {

class ReputationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class EligibilityError : public ReputationError {
public:
    using ReputationError::ReputationError;
};

class EvidenceError : public ReputationError {
public:
    using ReputationError::ReputationError;
};

class TaskConflictError : public ReputationError {
public:
    using ReputationError::ReputationError;
};

class SchedulerAlreadyRunningError : public ReputationError {
public:
    using ReputationError::ReputationError;
};

class CostBudgetExceededError : public ReputationError {
public:
    using ReputationError::ReputationError;
};

struct CostBudgetSnapshot {
    std::string agent_id;
    double limit_usd = 0.0;
    double spent_usd = 0.0;
    double reserved_usd = 0.0;
};

struct AgentIdentity {
    std::string agent_id;
    std::string provider;
    std::string model;
    std::string model_version;
    std::string prompt_version;
    std::string tool_config_version;
    std::string status;
    bool allow_leader = true;
    bool allow_inspector = true;
    std::size_t max_concurrent_tasks = 2;
    std::uint64_t max_provider_requests = 0;
    std::uint64_t provider_requests_used = 0;
};

struct AgentRuntimeSnapshot {
    std::string agent_id;
    std::string state;
    std::size_t queued_tasks = 0;
    std::size_t running_tasks = 0;
    std::optional<std::string> last_error_category;
    std::optional<std::string> last_error_at;
};

struct Performance {
    std::string agent_id;
    std::optional<std::string> domain;
    std::size_t evaluated_tasks = 0;
    std::size_t errors = 0;
    std::optional<double> error_rate;
    std::optional<double> weighted_score;
    bool eligible_for_governance = false;
    std::string reason;
};

struct InspectorPerformance {
    std::string agent_id;
    std::size_t evaluated_inspections = 0;
    std::size_t errors = 0;
    std::optional<double> error_rate;
    std::optional<double> precision;
    std::optional<double> recall;
    std::optional<double> specificity;
    bool eligible = false;
    std::string reason;
};

struct TaskRecord {
    std::string task_id;
    std::string agent_id;
    std::string state;
    std::string submitted_at;
    std::optional<std::string> started_at;
    std::optional<std::string> finished_at;
    std::optional<std::string> error_category;
    std::optional<std::uint64_t> prompt_tokens;
    std::optional<std::uint64_t> completion_tokens;
    std::optional<double> cost_usd;
};

struct TaskEvent {
    std::int64_t event_id = 0;
    std::string task_id;
    std::optional<std::string> from_state;
    std::string to_state;
    std::string occurred_at;
    std::optional<std::string> error_category;
};

struct ToolAuditRecord {
    std::int64_t event_id = 0;
    std::string request_id;
    std::string task_id;
    std::string agent_id;
    std::string tool_name;
    std::string arguments_sha256;
    std::string decision;
    std::optional<std::string> outcome_category;
    std::string occurred_at;
};

struct LeaderElection {
    std::string domain;
    std::optional<std::string> selected_agent_id;
    std::vector<std::string> challenger_agent_ids;
    std::string rationale;
};

struct InspectorRotation {
    std::string domain;
    std::optional<std::string> selected_agent_id;
    std::string rationale;
};

class AgentReputationStore {
public:
    explicit AgentReputationStore(std::string database_path);
    ~AgentReputationStore();
    AgentReputationStore(const AgentReputationStore&) = delete;
    AgentReputationStore& operator=(const AgentReputationStore&) = delete;

    [[nodiscard]] AgentIdentity register_agent(
        const std::string& agent_id, const std::string& provider,
        const std::string& model, const std::string& model_version,
        const std::string& prompt_version, const std::string& tool_config_version,
        bool allow_leader = true, bool allow_inspector = true);
    [[nodiscard]] AgentIdentity get_agent(const std::string& agent_id) const;
    [[nodiscard]] AgentRuntimeSnapshot
    runtime_snapshot(const std::string& agent_id) const;
    void set_status(const std::string& agent_id, const std::string& status);
    void set_agent_max_concurrent_tasks(const std::string& agent_id,
                                        std::size_t limit);
    void set_agent_max_provider_requests(const std::string& agent_id,
                                         std::uint64_t limit);
    [[nodiscard]] bool reserve_provider_request(const std::string& task_id,
                                                const std::string& agent_id);

    void record_assignment(const std::string& task_id, const std::string& agent_id,
                          const std::string& domain, int difficulty);
    [[nodiscard]] TaskRecord record_task_submission(
        const std::string& task_id, const std::string& agent_id,
        const std::string& domain, int difficulty, const std::string& submitted_at);
    [[nodiscard]] TaskRecord transition_task(
        const std::string& task_id, const std::string& state,
        const std::string& occurred_at,
        std::optional<std::string> error_category = std::nullopt,
        std::optional<std::uint64_t> prompt_tokens = std::nullopt,
        std::optional<std::uint64_t> completion_tokens = std::nullopt);
    [[nodiscard]] TaskRecord get_task_record(const std::string& task_id) const;
    [[nodiscard]] std::vector<TaskEvent> task_history(const std::string& task_id) const;
    void record_tool_audit(const std::string& request_id, const std::string& task_id,
                           const std::string& agent_id, const std::string& tool_name,
                           const std::string& arguments_sha256,
                           const std::string& decision,
                           std::optional<std::string> outcome_category,
                           const std::string& occurred_at);
    [[nodiscard]] std::vector<ToolAuditRecord>
    tool_audit_history(const std::string& task_id) const;
    [[nodiscard]] std::vector<TaskRecord> recover_incomplete_tasks(
        const std::string& occurred_at);
    [[nodiscard]] std::vector<TaskRecord>
    list_task_records(std::optional<std::string> agent_id = std::nullopt) const;
    void set_agent_cost_budget(const std::string& agent_id, double limit_usd);
    [[nodiscard]] bool reserve_task_cost(const std::string& task_id,
                                         const std::string& agent_id,
                                         double maximum_cost_usd);
    void settle_task_cost(const std::string& task_id, double actual_cost_usd);
    void release_task_cost(const std::string& task_id);
    [[nodiscard]] std::optional<CostBudgetSnapshot>
    cost_budget(const std::string& agent_id) const;

    void record_review(const std::string& task_id, const std::string& agent_id,
                       const std::string& evaluator_id, const std::string& verdict,
                       const std::string& severity, const std::string& evidence_sha256);
    void resolve_security_incident(const std::string& task_id,
                                   const std::string& agent_id);
    [[nodiscard]] Performance performance(
        const std::string& agent_id,
        std::optional<std::string> domain = std::nullopt) const;
    [[nodiscard]] LeaderElection elect_domain_leader(const std::string& domain);
    [[nodiscard]] std::optional<std::string>
    current_domain_leader(const std::string& domain);

    void record_inspection(const std::string& task_id,
                           const std::string& inspector_agent_id,
                           const std::string& inspected_agent_id,
                           const std::string& classification,
                           const std::string& evaluator_id,
                           const std::string& evidence_sha256);
    [[nodiscard]] InspectorPerformance
    inspector_performance(const std::string& agent_id) const;
    [[nodiscard]] InspectorRotation rotate_inspector(const std::string& domain);
    [[nodiscard]] std::optional<std::string> current_inspector(const std::string& domain);

    [[nodiscard]] int schema_version() const;
    [[nodiscard]] int database_mode() const;

private:
    friend class SchedulerLease;
    void revoke_ineligible_roles(const std::string& agent_id,
                                 const std::string& reason);
    void remove_role(const std::string& role, const std::string& domain,
                     const std::string& agent_id, const std::string& reason);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class SchedulerLease {
public:
    explicit SchedulerLease(const AgentReputationStore& store);
    ~SchedulerLease();
    SchedulerLease(const SchedulerLease&) = delete;
    SchedulerLease& operator=(const SchedulerLease&) = delete;

private:
    int descriptor_ = -1;
};

} // namespace ailinux::ai
