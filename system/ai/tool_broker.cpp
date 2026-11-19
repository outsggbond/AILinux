#include "tool_broker.hpp"

#include <openssl/evp.h>

#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <regex>
#include <sstream>

namespace ailinux::ai {
namespace {

const std::regex kIdentifier("^[A-Za-z][A-Za-z0-9_.-]{0,127}$");

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm value{};
    gmtime_r(&time, &value);
    std::array<char, 32> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &value);
    return buffer.data();
}

std::string sha256(const std::string& value) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_Digest(value.data(), value.size(), digest.data(), &length,
                   EVP_sha256(), nullptr) != 1) {
        throw ReputationError("cannot calculate tool argument digest");
    }
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < length; ++index) {
        result << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return result.str();
}

bool valid_type(const nlohmann::json& value, const std::string& type) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "integer") return value.is_number_integer();
    if (type == "number") return value.is_number();
    if (type == "boolean") return value.is_boolean();
    if (type == "null") return value.is_null();
    return false;
}

bool bounded_argument_tree(const nlohmann::json& value, std::size_t depth,
                           std::size_t& nodes, std::size_t& text_bytes) {
    if (depth > 16 || ++nodes > 4096) return false;
    if (value.is_string()) {
        const auto size = value.get_ref<const std::string&>().size();
        text_bytes += size;
        return size <= 16 * 1024 && text_bytes <= 64 * 1024;
    }
    if (value.is_object()) {
        for (auto item = value.begin(); item != value.end(); ++item) {
            text_bytes += item.key().size();
            if (item.key().size() > 128 || text_bytes > 64 * 1024 ||
                !bounded_argument_tree(item.value(), depth + 1, nodes,
                                       text_bytes)) {
                return false;
            }
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            if (!bounded_argument_tree(item, depth + 1, nodes, text_bytes)) {
                return false;
            }
        }
    }
    return true;
}

void validate_schema_definition(const nlohmann::json& schema,
                                std::size_t depth = 0) {
    if (depth > 16) {
        throw ToolConfigurationError("tool schema exceeds maximum nesting depth");
    }
    if (!schema.is_object() || !schema.contains("type") ||
        !schema["type"].is_string()) {
        throw ToolConfigurationError("tool schema must declare a JSON type");
    }
    const auto type = schema["type"].get<std::string>();
    static const std::unordered_set<std::string> supported_types = {
        "object", "array", "string", "integer", "number", "boolean", "null"};
    if (!supported_types.contains(type)) {
        throw ToolConfigurationError("tool schema uses an unsupported JSON type");
    }
    std::unordered_set<std::string> allowed_keys = {"type"};
    if (type == "object") {
        allowed_keys.insert("properties");
        allowed_keys.insert("required");
        allowed_keys.insert("additionalProperties");
    } else if (type == "array") {
        allowed_keys.insert("items");
    }
    for (auto keyword = schema.begin(); keyword != schema.end(); ++keyword) {
        if (!allowed_keys.contains(keyword.key())) {
            throw ToolConfigurationError("tool schema uses an unsupported keyword");
        }
    }
    if (type == "object") {
        if (!schema.contains("properties") || !schema["properties"].is_object()) {
            throw ToolConfigurationError("object schema must declare properties");
        }
        if (schema["properties"].size() > 64) {
            throw ToolConfigurationError("object schema has too many properties");
        }
        if (schema.contains("additionalProperties") &&
            !schema["additionalProperties"].is_boolean()) {
            throw ToolConfigurationError("additionalProperties must be boolean");
        }
        std::unordered_set<std::string> property_names;
        for (auto property = schema["properties"].begin();
             property != schema["properties"].end(); ++property) {
            property_names.insert(property.key());
            validate_schema_definition(property.value(), depth + 1);
        }
        if (schema.contains("required")) {
            if (!schema["required"].is_array()) {
                throw ToolConfigurationError("required must be an array");
            }
            std::unordered_set<std::string> required_names;
            if (schema["required"].size() > 64) {
                throw ToolConfigurationError("object schema has too many required properties");
            }
            for (const auto& required : schema["required"]) {
                if (!required.is_string() ||
                    !property_names.contains(required.get<std::string>()) ||
                    !required_names.insert(required.get<std::string>()).second) {
                    throw ToolConfigurationError(
                        "required must contain unique declared property names");
                }
            }
        }
    } else if (type == "array") {
        if (!schema.contains("items")) {
            throw ToolConfigurationError("array schema must declare item type");
        }
        validate_schema_definition(schema["items"], depth + 1);
    }
}

bool matches_schema(const nlohmann::json& value,
                    const nlohmann::json& schema, std::size_t depth = 0) {
    if (depth > 16) return false;
    const auto type = schema["type"].get<std::string>();
    if (!valid_type(value, type)) return false;
    if (type == "object") {
        for (const auto& required : schema.value(
                 "required", nlohmann::json::array())) {
            if (!value.contains(required.get<std::string>())) return false;
        }
        const bool allow_extra = schema.value("additionalProperties", false);
        for (auto item = value.begin(); item != value.end(); ++item) {
            if (!schema["properties"].contains(item.key())) {
                if (!allow_extra) return false;
            } else if (!matches_schema(item.value(),
                                       schema["properties"][item.key()],
                                       depth + 1)) {
                return false;
            }
        }
    } else if (type == "array") {
        for (const auto& item : value) {
            if (!matches_schema(item, schema["items"], depth + 1)) return false;
        }
    }
    return true;
}

} // namespace

ToolExecutionError::ToolExecutionError(std::string category_value)
    : std::runtime_error("tool handler failed"), category(std::move(category_value)) {
    if (!std::regex_match(category, kIdentifier)) {
        throw std::invalid_argument("tool error category must be a safe identifier");
    }
}

ToolBroker::ToolBroker(AgentReputationStore& reputation) : reputation_(reputation) {}

void ToolBroker::register_tool(ToolDefinition definition, Handler handler) {
    if (!std::regex_match(definition.name, kIdentifier) || !handler) {
        throw ToolConfigurationError("tool requires a safe name and handler");
    }
    validate_schema_definition(definition.arguments_schema);
    if (definition.arguments_schema["type"] != "object") {
        throw ToolConfigurationError("tool arguments schema must be an object");
    }
    if (definition.risk != ToolRisk::read_only &&
        definition.risk != ToolRisk::state_change &&
        definition.risk != ToolRisk::privileged) {
        throw ToolConfigurationError("tool risk classification is invalid");
    }
    const auto name = definition.name;
    std::lock_guard lock(mutex_);
    if (!tools_.emplace(name,
                        RegisteredTool{std::move(definition), std::move(handler)})
             .second) {
        throw ToolConfigurationError("tool name is already registered");
    }
}

ToolExecutionResult ToolBroker::invoke(
    const ToolAuthorization& authorization, const std::string& tool_name,
    const nlohmann::json& arguments) {
    const auto canonical_arguments = arguments.dump(-1, ' ', true);
    const auto digest = sha256(canonical_arguments);
    auto audit = [&](const std::string& decision,
                     std::optional<std::string> category = std::nullopt) {
        reputation_.record_tool_audit(
            authorization.request_id, authorization.task_id,
            authorization.agent_id, tool_name, digest, decision,
            std::move(category), utc_now());
    };
    auto deny = [&](const std::string& category) {
        audit("denied", category);
        return ToolExecutionResult{"denied", category, std::nullopt};
    };

    std::size_t nodes = 0;
    std::size_t text_bytes = 0;
    if (canonical_arguments.size() > 64 * 1024 ||
        !bounded_argument_tree(arguments, 0, nodes, text_bytes)) {
        return deny("arguments_out_of_bounds");
    }

    std::optional<RegisteredTool> registered_tool;
    {
        std::lock_guard lock(mutex_);
        const auto registered = tools_.find(tool_name);
        if (registered != tools_.end()) {
            registered_tool = registered->second;
        }
    }
    if (!registered_tool) return deny("tool_not_registered");
    const auto& tool = *registered_tool;
    if (!std::regex_match(authorization.request_id, kIdentifier) ||
        !std::regex_match(authorization.task_id, kIdentifier) ||
        !std::regex_match(authorization.agent_id, kIdentifier)) {
        return deny("invalid_authorization");
    }

    AgentIdentity agent;
    TaskRecord task;
    try {
        agent = reputation_.get_agent(authorization.agent_id);
    } catch (const std::out_of_range&) {
        return deny("unknown_agent");
    }
    try {
        task = reputation_.get_task_record(authorization.task_id);
    } catch (const std::out_of_range&) {
        return deny("unknown_task");
    }
    if (agent.status != "ready" || task.agent_id != authorization.agent_id ||
        task.state != "running") {
        return deny("agent_or_task_not_active");
    }
    if (!authorization.allowed_tools.contains(tool_name)) {
        return deny("tool_not_authorized");
    }
    if (tool.definition.risk == ToolRisk::privileged) {
        return deny("privileged_operations_disabled");
    }
    if (tool.definition.risk == ToolRisk::state_change &&
        !authorization.confirmation_granted) {
        audit("approval-required", "confirmation_required");
        return {"approval-required", "confirmation_required", std::nullopt};
    }
    if (!matches_schema(arguments, tool.definition.arguments_schema)) {
        return deny("invalid_arguments");
    }

    audit("started");
    try {
        auto result = tool.handler(arguments);
        audit("executed", "success");
        return {"executed", "success", std::move(result)};
    } catch (const ToolExecutionError& error) {
        audit("failed", error.category);
        throw;
    } catch (...) {
        audit("failed", "handler_error");
        throw;
    }
}

} // namespace ailinux::ai
