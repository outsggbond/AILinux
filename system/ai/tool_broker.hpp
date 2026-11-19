#pragma once

#include "reputation.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace ailinux::ai {

class ToolConfigurationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ToolExecutionError : public std::runtime_error {
public:
    explicit ToolExecutionError(std::string category);
    const std::string category;
};

enum class ToolRisk { read_only, state_change, privileged };

struct ToolDefinition {
    std::string name;
    nlohmann::json arguments_schema;
    ToolRisk risk = ToolRisk::read_only;
};

struct ToolAuthorization {
    std::string request_id;
    std::string task_id;
    std::string agent_id;
    std::unordered_set<std::string> allowed_tools;
    bool confirmation_granted = false;
};

struct ToolExecutionResult {
    std::string decision;
    std::optional<std::string> outcome_category;
    std::optional<nlohmann::json> result;
};

class ToolBroker {
public:
    using Handler = std::function<nlohmann::json(const nlohmann::json&)>;

    explicit ToolBroker(AgentReputationStore& reputation);
    void register_tool(ToolDefinition definition, Handler handler);
    [[nodiscard]] ToolExecutionResult invoke(
        const ToolAuthorization& authorization, const std::string& tool_name,
        const nlohmann::json& arguments);

private:
    struct RegisteredTool {
        ToolDefinition definition;
        Handler handler;
    };

    AgentReputationStore& reputation_;
    std::mutex mutex_;
    std::unordered_map<std::string, RegisteredTool> tools_;
};

} // namespace ailinux::ai
