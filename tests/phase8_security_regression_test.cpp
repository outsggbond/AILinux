#include "reputation.hpp"
#include "tool_broker.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

int main() {
    try {
        char template_name[] = "/tmp/ailinux-phase8-security-XXXXXX";
        char* parent = mkdtemp(template_name);
        if (parent == nullptr) throw std::runtime_error("cannot create security test directory");
        chmod(parent, 0700);

        ailinux::ai::AgentReputationStore store(
            std::string(parent) + "/security.sqlite3");
        static_cast<void>(store.register_agent(
            "agent-a", "provider-a", "model-a", "v1", "prompt-a", "tools-a"));
        static_cast<void>(store.record_task_submission(
            "task-a", "agent-a", "software-development", 1,
            "2026-11-21T12:00:00Z"));
        static_cast<void>(store.transition_task(
            "task-a", "running", "2026-11-21T12:00:01Z"));

        ailinux::ai::ToolBroker broker(store);
        const nlohmann::json schema = {
            {"type", "object"},
            {"properties", nlohmann::json::object()},
            {"additionalProperties", false}};
        broker.register_tool(
            {"system.read_status", schema, ailinux::ai::ToolRisk::read_only},
            [](const nlohmann::json&) { return nlohmann::json{{"ok", true}}; });
        broker.register_tool(
            {"system.restart_service", schema, ailinux::ai::ToolRisk::state_change},
            [](const nlohmann::json&) { return nlohmann::json{{"ok", true}}; });
        broker.register_tool(
            {"system.install_package", schema, ailinux::ai::ToolRisk::privileged},
            [](const nlohmann::json&) { return nlohmann::json{{"ok", true}}; });

        ailinux::ai::ToolAuthorization denied{
            "denied-request", "task-a", "agent-a", {}, false};
        const auto unauthorized = broker.invoke(denied, "system.read_status", nlohmann::json::object());
        if (unauthorized.decision != "denied" ||
            unauthorized.outcome_category != "tool_not_authorized") {
            throw std::runtime_error("unauthorized tool was not denied");
        }

        denied.allowed_tools = {"system.restart_service"};
        const auto approval = broker.invoke(
            denied, "system.restart_service", nlohmann::json::object());
        if (approval.decision != "approval-required") {
            throw std::runtime_error("state-changing tool bypassed confirmation");
        }

        denied.allowed_tools.insert("system.install_package");
        denied.confirmation_granted = true;
        const auto privileged = broker.invoke(
            denied, "system.install_package", nlohmann::json::object());
        if (privileged.decision != "denied" ||
            privileged.outcome_category != "privileged_operations_disabled") {
            throw std::runtime_error("privileged tool was not disabled");
        }

        denied.allowed_tools = {"system.read_status"};
        denied.confirmation_granted = false;
        for (int index = 0; index < 1000; ++index) {
            denied.request_id = "bounded-request-" + std::to_string(index);
            const auto result = broker.invoke(
                denied, "system.read_status", nlohmann::json::object());
            if (result.decision != "executed") {
                throw std::runtime_error("authorized bounded read was denied");
        }
        }

        const auto audit = store.tool_audit_history("task-a");
        if (audit.size() != 2003) {
            throw std::runtime_error(
                "security audit event count mismatch: " +
                std::to_string(audit.size()));
        }

        std::filesystem::remove_all(parent);
        std::cout << "Phase 8 security regression passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
