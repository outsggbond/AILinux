#include "council_manager.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <exception>
#include <optional>
#include <set>

namespace ailinux::ai {
namespace {

constexpr std::size_t kMaxCouncilRequestBytes = 8 * 1024 * 1024;
std::atomic<std::uint64_t> task_sequence{0};

std::string timestamp_now() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm value{};
    gmtime_r(&time, &value);
    std::array<char, 32> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ",
                      &value) == 0) {
        throw CouncilManagerError("cannot create Council timestamp");
    }
    return buffer.data();
}

ChatRequest request_for(const AgentIdentity& agent, const std::string& system,
                        const std::string& user) {
    ChatRequest request{
        agent.model,
        {ChatMessage{"system", system, std::nullopt, {}},
         ChatMessage{"user", user, std::nullopt, {}}},
        {},
        std::nullopt,
        std::nullopt};
    if (request.to_json().dump().size() > kMaxCouncilRequestBytes) {
        throw CouncilManagerError("Council Provider request exceeds its size limit");
    }
    return request;
}

std::string required_string(const nlohmann::json& value, const char* field) {
    if (!value.is_object() || !value.contains(field) ||
        !value[field].is_string()) {
        throw CouncilManagerError(std::string("Council response is missing field: ") +
                                  field);
    }
    const auto result = value[field].get<std::string>();
    if (result.empty() || result.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw CouncilManagerError(std::string("Council response has empty field: ") +
                                  field);
    }
    return result;
}

nlohmann::json response_json(const ChatResponse& response) {
    if (!response.tool_calls.empty() || !response.content.has_value()) {
        throw CouncilManagerError(
            "Council phase requires structured text, not tool calls");
    }
    try {
        const auto parsed = nlohmann::json::parse(*response.content);
        if (!parsed.is_object()) {
            throw CouncilManagerError("Council response must be a JSON object");
        }
        return parsed;
    } catch (const nlohmann::json::parse_error&) {
        throw CouncilManagerError("Council response is not valid JSON");
    }
}

CouncilOpinion parse_opinion(const ChatResponse& response) {
    const auto value = response_json(response);
    return {required_string(value, "understanding"),
            required_string(value, "technical_analysis"),
            required_string(value, "recommendation"),
            required_string(value, "risks"),
            required_string(value, "implementation_steps"),
            required_string(value, "impact"),
            required_string(value, "tests"),
            required_string(value, "rollback_plan")};
}

std::pair<std::string, std::string>
parse_cross_review(const ChatResponse& response) {
    const auto value = response_json(response);
    auto verdict = required_string(value, "verdict");
    if (verdict != "support" && verdict != "concern" && verdict != "reject") {
        throw CouncilManagerError("Council response has an invalid review verdict");
    }
    return {std::move(verdict), required_string(value, "findings")};
}

CouncilDecision parse_decision(const ChatResponse& response) {
    const auto value = response_json(response);
    CouncilDecision decision{
        required_string(value, "final_plan"),
        required_string(value, "rationale"),
        required_string(value, "rejected_alternatives"),
        required_string(value, "risk_level"),
        required_string(value, "execution_plan"),
        required_string(value, "test_criteria"),
        required_string(value, "rollback_conditions")};
    if (decision.risk_level != "low" && decision.risk_level != "moderate" &&
        decision.risk_level != "high" && decision.risk_level != "critical") {
        throw CouncilManagerError("Council decision has an invalid risk level");
    }
    return decision;
}

std::pair<std::string, std::string>
parse_inspection(const ChatResponse& response) {
    const auto value = response_json(response);
    auto verdict = required_string(value, "verdict");
    if (verdict != "verified" && verdict != "changes-requested" &&
        verdict != "rejected") {
        throw CouncilManagerError("Inspector response has an invalid verdict");
    }
    return {std::move(verdict), required_string(value, "report")};
}

std::string opinion_json(const CouncilOpinion& opinion) {
    return nlohmann::json{
               {"understanding", opinion.understanding},
               {"technical_analysis", opinion.technical_analysis},
               {"recommendation", opinion.recommendation},
               {"risks", opinion.risks},
               {"implementation_steps", opinion.implementation_steps},
               {"impact", opinion.impact},
               {"tests", opinion.tests},
               {"rollback_plan", opinion.rollback_plan}}
        .dump();
}

std::string evidence_sha256(const std::string& evidence) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_Digest(evidence.data(), evidence.size(), digest.data(), &length,
                   EVP_sha256(), nullptr) != 1) {
        throw CouncilManagerError("cannot calculate Inspector evidence digest");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(static_cast<std::size_t>(length) * 2);
    for (unsigned int index = 0; index < length; ++index) {
        result.push_back(hex[digest[index] >> 4]);
        result.push_back(hex[digest[index] & 0x0f]);
    }
    return result;
}

std::string scheduler_task_id(const std::string& session_id,
                              const std::string& phase,
                              const std::string& agent_id) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto material =
        session_id + "|" + phase + "|" + agent_id + "|" +
        std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(now)
                           .count()) +
        "|" + std::to_string(task_sequence.fetch_add(1));
    return "council-" + evidence_sha256(material).substr(0, 48);
}

ChatResponse run_agent_task(AgentScheduler& scheduler,
                            const CouncilSession& session,
                            const AgentIdentity& agent,
                            const std::string& phase,
                            ChatRequest request) {
    auto result = scheduler
                      .submit({scheduler_task_id(session.session_id, phase,
                                                 agent.agent_id),
                               agent.agent_id, session.domain, 1,
                               std::move(request)})
                      .get();
    if (result.status.state != "completed" || !result.response.has_value()) {
        const auto category = result.status.error_category.value_or(result.status.state);
        throw CouncilManagerError("Council Agent task " + result.status.task_id +
                                  " failed: " + category);
    }
    return std::move(*result.response);
}

std::vector<ChatResponse>
await_agent_tasks(std::vector<std::future<AgentTaskResult>>& jobs,
                  const std::string& phase) {
    std::vector<ChatResponse> responses;
    responses.reserve(jobs.size());
    std::optional<std::string> failure;
    std::exception_ptr unexpected_failure;
    for (auto& job : jobs) {
        try {
            auto result = job.get();
            if (result.status.state != "completed" ||
                !result.response.has_value()) {
                if (!failure.has_value()) {
                    failure = result.status.task_id + ":" +
                              result.status.error_category.value_or(
                                  result.status.state);
                }
                continue;
            }
            responses.push_back(std::move(*result.response));
        } catch (...) {
            if (!unexpected_failure) {
                unexpected_failure = std::current_exception();
            }
        }
    }
    if (unexpected_failure) {
        std::rethrow_exception(unexpected_failure);
    }
    if (failure.has_value()) {
        throw CouncilManagerError("Council " + phase + " task failed: " +
                                  *failure);
    }
    return responses;
}

std::string analysis_prompt(const CouncilSession& session) {
    return "Analyze this task independently. Do not assume another participant's "
           "conclusion. Return only a JSON object with non-empty string fields "
           "understanding, technical_analysis, recommendation, risks, "
           "implementation_steps, impact, tests, rollback_plan. Task ID: " +
           session.task_id + "\nDomain: " + session.domain +
           "\nTask: " + session.task_summary;
}

} // namespace

CouncilManager::CouncilManager(CouncilWorkflow& workflow,
                               AgentReputationStore& reputation,
                               AgentScheduler& scheduler)
    : workflow_(workflow), reputation_(reputation), scheduler_(scheduler) {}

void CouncilManager::deliberate(const std::string& session_id) {
    auto session = workflow_.get_session(session_id);
    if (session.phase == "intake") {
        workflow_.begin_analysis(session_id, timestamp_now());
        session = workflow_.get_session(session_id);
    }

    const auto participants = workflow_.participants(session_id);
    std::vector<CouncilParticipant> voters;
    for (const auto& participant : participants) {
        if (participant.role != "inspector") {
            voters.push_back(participant);
        }
    }
    if (voters.size() < 2) {
        throw CouncilManagerError("Council deliberation requires two participants");
    }

    if (session.phase == "independent-analysis") {
        const auto existing = workflow_.opinion_records(session_id);
        std::set<std::string> completed;
        for (const auto& record : existing) completed.insert(record.agent_id);
        std::vector<const CouncilParticipant*> missing;
        std::vector<std::future<AgentTaskResult>> jobs;
        for (const auto& participant : voters) {
            if (completed.contains(participant.agent_id)) continue;
            const auto identity = reputation_.get_agent(participant.agent_id);
            if (identity.status != "ready") {
                throw CouncilManagerError("Council participant is not ready");
            }
            missing.push_back(&participant);
            jobs.push_back(scheduler_.submit(
                {scheduler_task_id(session.session_id, "analysis",
                                   identity.agent_id),
                 identity.agent_id, session.domain, 1,
                 request_for(identity, "AILINUX_COUNCIL_INDEPENDENT_ANALYSIS",
                             analysis_prompt(session))}));
        }
        const auto responses = await_agent_tasks(jobs, "analysis");
        std::vector<CouncilOpinion> parsed;
        parsed.reserve(responses.size());
        for (const auto& response : responses) parsed.push_back(parse_opinion(response));
        for (std::size_t index = 0; index < missing.size(); ++index) {
            workflow_.submit_opinion(session_id, missing[index]->agent_id,
                                     parsed[index], timestamp_now());
        }
        workflow_.begin_cross_review(session_id, timestamp_now());
        session = workflow_.get_session(session_id);
    }

    if (session.phase == "cross-review") {
        const auto opinions = workflow_.opinion_records(session_id);
        if (opinions.size() != voters.size()) {
            throw CouncilManagerError("Council is missing independent opinions");
        }
        const auto existing = workflow_.cross_reviews(session_id);
        std::set<std::string> reviewed_agents;
        for (const auto& review : existing) {
            reviewed_agents.insert(review.reviewer_agent_id);
        }
        std::vector<const CouncilParticipant*> reviewers;
        std::vector<std::future<AgentTaskResult>> jobs;
        for (std::size_t index = 0; index < voters.size(); ++index) {
            const auto& reviewer = voters[index];
            if (reviewed_agents.contains(reviewer.agent_id)) continue;
            const auto& target =
                voters[(index + 1) % voters.size()];
            const auto target_opinion = std::find_if(
                opinions.begin(), opinions.end(), [&](const auto& record) {
                    return record.agent_id == target.agent_id;
                });
            if (target_opinion == opinions.end()) {
                throw CouncilManagerError("Council peer analysis is missing");
            }
            const auto identity = reputation_.get_agent(reviewer.agent_id);
            if (identity.status != "ready") {
                throw CouncilManagerError("Council reviewer is not ready");
            }
            const auto prompt =
                "Review this peer's independent proposal. Return only a JSON "
                "object with verdict (support, concern, or reject) and a non-empty "
                "findings string. Task: " +
                session.task_summary + "\nPeer Agent: " + target.agent_id +
                "\nPeer proposal: " + opinion_json(target_opinion->opinion);
            reviewers.push_back(&reviewer);
            jobs.push_back(scheduler_.submit(
                {scheduler_task_id(session.session_id, "review",
                                   identity.agent_id),
                 identity.agent_id, session.domain, 1,
                 request_for(identity, "AILINUX_COUNCIL_CROSS_REVIEW", prompt)}));
        }
        const auto responses = await_agent_tasks(jobs, "cross-review");
        std::vector<std::pair<std::string, std::string>> parsed;
        parsed.reserve(jobs.size());
        for (const auto& response : responses) {
            parsed.push_back(parse_cross_review(response));
        }
        for (std::size_t index = 0; index < reviewers.size(); ++index) {
            const auto voter = std::find_if(
                voters.begin(), voters.end(), [&](const auto& candidate) {
                    return candidate.agent_id == reviewers[index]->agent_id;
                });
            const auto target = static_cast<std::size_t>(
                std::distance(voters.begin(), voter) + 1) % voters.size();
            workflow_.submit_cross_review(
                session_id, reviewers[index]->agent_id,
                voters[target].agent_id, parsed[index].first,
                parsed[index].second, timestamp_now());
        }
        workflow_.begin_leader_decision(session_id, timestamp_now());
        session = workflow_.get_session(session_id);
    }

    if (session.phase == "leader-decision") {
        const auto identity = reputation_.get_agent(session.leader_agent_id);
        if (identity.status != "ready") {
            throw CouncilManagerError("Council Leader is not ready");
        }
        const auto opinions = workflow_.opinion_records(session_id);
        const auto reviews = workflow_.cross_reviews(session_id);
        nlohmann::json evidence = nlohmann::json::array();
        for (const auto& record : opinions) {
            evidence.push_back({{"agent_id", record.agent_id},
                                {"opinion", nlohmann::json::parse(
                                                opinion_json(record.opinion))}});
        }
        nlohmann::json review_records = nlohmann::json::array();
        for (const auto& review : reviews) {
            review_records.push_back(
                {{"reviewer_agent_id", review.reviewer_agent_id},
                 {"target_agent_id", review.target_agent_id},
                 {"verdict", review.verdict},
                 {"findings", review.findings}});
        }
        const auto prompt =
            "Synthesize the independent opinions and dissenting peer reviews. "
            "Do not decide by majority vote. Return only a JSON object with "
            "non-empty string fields final_plan, rationale, "
            "rejected_alternatives, risk_level (low, moderate, high, critical), "
            "execution_plan, test_criteria, rollback_conditions. Task: " +
            session.task_summary + "\nOpinions: " + evidence.dump() +
            "\nCross-reviews: " + review_records.dump();
        const auto response = run_agent_task(
            scheduler_, session, identity, "leader-decision",
            request_for(identity, "AILINUX_COUNCIL_LEADER_DECISION", prompt));
        const auto decision = parse_decision(response);
        workflow_.submit_decision(session_id, session.leader_agent_id, decision,
                                  timestamp_now());
        session = workflow_.get_session(session_id);
    }

    if (session.phase != "execution") {
        throw CouncilManagerError(
            "Council deliberation stopped before the execution phase");
    }
}

void CouncilManager::inspect_execution(const std::string& session_id) {
    auto session = workflow_.get_session(session_id);
    if (session.phase == "execution") {
        workflow_.begin_inspection(session_id, session.leader_agent_id,
                                   timestamp_now());
        session = workflow_.get_session(session_id);
    }
    if (session.phase != "inspection") {
        throw CouncilManagerError(
            "Council session is not ready for Inspector review");
    }
    const auto identity = reputation_.get_agent(session.inspector_agent_id);
    if (identity.status != "ready") {
        throw CouncilManagerError("Council Inspector is not ready");
    }
    const auto selected_decision = workflow_.decision(session_id);
    if (!selected_decision.has_value()) {
        throw CouncilManagerError("Council has no Leader decision to inspect");
    }
    const auto execution_reports = workflow_.execution_reports(session_id);
    if (execution_reports.empty()) {
        throw CouncilManagerError("Council has no execution evidence to inspect");
    }
    const auto current_cycle = std::max_element(
        execution_reports.begin(), execution_reports.end(),
        [](const auto& left, const auto& right) {
            return left.execution_cycle < right.execution_cycle;
        })->execution_cycle;
    nlohmann::json reports = nlohmann::json::array();
    for (const auto& report : execution_reports) {
        if (report.execution_cycle != current_cycle) continue;
        reports.push_back({{"agent_id", report.agent_id},
                           {"outcome", report.outcome},
                           {"summary", report.summary},
                           {"error_category",
                            report.error_category.has_value()
                                ? nlohmann::json(*report.error_category)
                                : nlohmann::json(nullptr)}});
    }
    const auto decision_data =
        nlohmann::json{{"final_plan", selected_decision->final_plan},
                       {"rationale", selected_decision->rationale},
                       {"risk_level", selected_decision->risk_level},
                       {"execution_plan", selected_decision->execution_plan},
                       {"test_criteria", selected_decision->test_criteria},
                       {"rollback_conditions",
                        selected_decision->rollback_conditions}};
    const auto evidence =
        "Task: " + session.task_summary + "\nDecision: " +
        decision_data.dump() + "\nExecution reports: " + reports.dump();
    const auto prompt =
        "Independently inspect whether execution evidence satisfies the Leader "
        "plan, tests, safety constraints, and rollback conditions. Do not rely "
        "on the Leader's assertions alone. Return only a JSON object with "
        "verdict (verified, changes-requested, or rejected) and non-empty "
        "report. Evidence:\n" +
        evidence;
    const auto response = run_agent_task(
        scheduler_, session, identity, "inspector",
        request_for(identity, "AILINUX_COUNCIL_INSPECTOR", prompt));
    const auto [verdict, report] = parse_inspection(response);
    workflow_.submit_inspection(session_id, session.inspector_agent_id, verdict,
                                 report,
                                 evidence_sha256(evidence + "\nVerdict: " +
                                                 verdict + "\nReport: " + report),
                                 timestamp_now());
}

} // namespace ailinux::ai
