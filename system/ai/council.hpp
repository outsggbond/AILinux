#pragma once

#include "reputation.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ailinux::ai {

class CouncilError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct CouncilSession {
    std::string session_id;
    std::string task_id;
    std::string task_summary;
    std::string domain;
    std::string leader_agent_id;
    std::string inspector_agent_id;
    std::string phase;
    std::string created_at;
    std::string updated_at;
};

struct CouncilParticipant {
    std::string agent_id;
    std::string role;
};

struct CouncilOpinion {
    std::string understanding;
    std::string technical_analysis;
    std::string recommendation;
    std::string risks;
    std::string implementation_steps;
    std::string impact;
    std::string tests;
    std::string rollback_plan;
};

struct CouncilOpinionRecord {
    std::string agent_id;
    CouncilOpinion opinion;
};

struct CouncilCrossReview {
    std::string reviewer_agent_id;
    std::string target_agent_id;
    std::string verdict;
    std::string findings;
    std::string created_at;
};

struct CouncilDecision {
    std::string final_plan;
    std::string rationale;
    std::string rejected_alternatives;
    std::string risk_level;
    std::string execution_plan;
    std::string test_criteria;
    std::string rollback_conditions;
};

struct CouncilExecutionReport {
    std::uint64_t execution_cycle = 0;
    std::string agent_id;
    std::string outcome;
    std::string summary;
    std::optional<std::string> error_category;
    std::string created_at;
};

struct CouncilInspection {
    std::uint64_t execution_cycle = 0;
    std::string verdict;
    std::string report;
    std::string evidence_sha256;
    std::string reviewed_at;
};

struct CouncilPhaseEvent {
    std::int64_t event_id = 0;
    std::string from_phase;
    std::string to_phase;
    std::optional<std::string> actor_agent_id;
    std::string occurred_at;
};

class CouncilWorkflow {
public:
    CouncilWorkflow(AgentReputationStore& reputation, std::string database_path);
    ~CouncilWorkflow();
    CouncilWorkflow(const CouncilWorkflow&) = delete;
    CouncilWorkflow& operator=(const CouncilWorkflow&) = delete;

    [[nodiscard]] CouncilSession create_session(
        const std::string& session_id, const std::string& task_id,
        const std::string& task_summary, const std::string& domain,
        const std::string& leader_agent_id,
        const std::string& inspector_agent_id,
        const std::vector<std::string>& member_agent_ids,
        const std::string& occurred_at);
    [[nodiscard]] CouncilSession get_session(const std::string& session_id) const;
    [[nodiscard]] std::vector<CouncilParticipant>
    participants(const std::string& session_id) const;
    [[nodiscard]] std::vector<CouncilOpinion>
    opinions(const std::string& session_id) const;
    [[nodiscard]] std::vector<CouncilOpinionRecord>
    opinion_records(const std::string& session_id) const;
    [[nodiscard]] std::vector<CouncilCrossReview>
    cross_reviews(const std::string& session_id) const;
    [[nodiscard]] std::optional<CouncilDecision>
    decision(const std::string& session_id) const;
    [[nodiscard]] std::vector<CouncilExecutionReport>
    execution_reports(const std::string& session_id) const;
    [[nodiscard]] std::optional<CouncilInspection>
    inspection(const std::string& session_id) const;
    [[nodiscard]] std::vector<CouncilInspection>
    inspection_history(const std::string& session_id) const;
    [[nodiscard]] std::vector<CouncilPhaseEvent>
    phase_history(const std::string& session_id) const;

    void begin_analysis(const std::string& session_id, const std::string& occurred_at);
    void submit_opinion(const std::string& session_id,
                        const std::string& agent_id,
                        const CouncilOpinion& opinion,
                        const std::string& occurred_at);
    void begin_cross_review(const std::string& session_id,
                            const std::string& occurred_at);
    void submit_cross_review(const std::string& session_id,
                             const std::string& reviewer_agent_id,
                             const std::string& target_agent_id,
                             const std::string& verdict,
                             const std::string& findings,
                             const std::string& occurred_at);
    void begin_leader_decision(const std::string& session_id,
                               const std::string& occurred_at);
    void submit_decision(const std::string& session_id,
                         const std::string& leader_agent_id,
                         const CouncilDecision& decision,
                         const std::string& occurred_at);
    void submit_execution_report(const std::string& session_id,
                                 const std::string& agent_id,
                                 const std::string& outcome,
                                 const std::string& summary,
                                 std::optional<std::string> error_category,
                                 const std::string& occurred_at);
    void begin_inspection(const std::string& session_id,
                          const std::string& leader_agent_id,
                          const std::string& occurred_at);
    void submit_inspection(const std::string& session_id,
                           const std::string& inspector_agent_id,
                           const std::string& verdict,
                           const std::string& report,
                           const std::string& evidence_sha256,
                           const std::string& occurred_at);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ailinux::ai
