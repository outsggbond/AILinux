#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace ailinux::ai {

class ProviderConfigurationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ProviderProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ProviderCancelledError : public std::runtime_error {
public:
    ProviderCancelledError(std::string message, bool request_started = false)
        : std::runtime_error(std::move(message)), request_started(request_started) {}

    const bool request_started;
};

class ProviderTaskTimeoutError : public std::runtime_error {
public:
    ProviderTaskTimeoutError(std::string message, bool request_started = false)
        : std::runtime_error(std::move(message)), request_started(request_started) {}

    const bool request_started;
};

class ProviderError : public std::runtime_error {
public:
    ProviderError(std::string provider, std::string category, std::string message,
                  long status = 0, std::optional<double> retry_after = std::nullopt,
                  bool retryable = false);

    const std::string provider;
    const std::string category;
    const long status;
    const std::optional<double> retry_after;
    const bool retryable;
};

struct ChatMessage {
    std::string role;
    std::optional<std::string> content;
    std::optional<std::string> tool_call_id;
    std::vector<nlohmann::json> tool_calls;

    [[nodiscard]] nlohmann::json to_json() const;
};

struct ChatRequest {
    std::string model;
    std::vector<ChatMessage> messages;
    std::vector<nlohmann::json> tools;
    std::optional<nlohmann::json> tool_choice;
    std::optional<std::size_t> max_tokens;

    [[nodiscard]] nlohmann::json to_json() const;
};

struct ToolCall {
    std::string call_id;
    std::string name;
    nlohmann::json arguments;
};

struct ChatResponse {
    std::string response_id;
    std::string model;
    std::optional<std::string> content;
    std::vector<ToolCall> tool_calls;
    std::optional<std::string> finish_reason;
    std::optional<std::uint64_t> prompt_tokens;
    std::optional<std::uint64_t> completion_tokens;
};

struct ProviderConfig {
    std::string model;
    std::string credential_name;
    std::string base_url;
    double timeout_seconds = 30.0;
    std::size_t max_response_bytes = 2 * 1024 * 1024;
    std::size_t max_completion_tokens = 2048;
    std::size_t max_prompt_bytes = 128 * 1024;
    std::optional<double> input_cost_per_million_tokens_usd = std::nullopt;
    std::optional<double> output_cost_per_million_tokens_usd = std::nullopt;
};

class ChatCompletionProvider {
public:
    ChatCompletionProvider(std::string provider_name, std::string default_base_url,
                           std::string default_credential, ProviderConfig config);
    virtual ~ChatCompletionProvider() = default;

    [[nodiscard]] ChatResponse complete(
        const ChatRequest& request,
        const std::atomic<bool>* cancellation = nullptr,
        std::optional<std::chrono::steady_clock::time_point> deadline =
            std::nullopt) const;
    [[nodiscard]] const std::string& name() const noexcept { return provider_name_; }
    [[nodiscard]] const ProviderConfig& config() const noexcept { return config_; }

private:
    [[nodiscard]] ChatResponse parse_response(const nlohmann::json& payload) const;
    [[nodiscard]] std::string read_credential() const;

    std::string provider_name_;
    std::string default_base_url_;
    std::string default_credential_;
    ProviderConfig config_;
};

class OpenAIProvider final : public ChatCompletionProvider {
public:
    explicit OpenAIProvider(ProviderConfig config);
};

class DeepSeekProvider final : public ChatCompletionProvider {
public:
    explicit DeepSeekProvider(ProviderConfig config);
};

class KimiProvider final : public ChatCompletionProvider {
public:
    explicit KimiProvider(ProviderConfig config);
};

class ProviderPool {
public:
    explicit ProviderPool(std::map<std::string, const ChatCompletionProvider*> providers,
                          std::size_t max_concurrent_per_provider = 4);

    [[nodiscard]] ChatResponse complete(const std::string& provider,
                                        const ChatRequest& request,
                                        const std::atomic<bool>* cancellation = nullptr,
                                        std::optional<std::chrono::steady_clock::time_point>
                                            deadline = std::nullopt) const;
    [[nodiscard]] std::vector<ChatResponse>
    complete_many(const std::vector<std::pair<std::string, ChatRequest>>& jobs) const;
    [[nodiscard]] double maximum_cost_usd(
        const std::string& provider, const ChatRequest& request) const;
    [[nodiscard]] double actual_cost_usd(
        const std::string& provider, const ChatRequest& request,
        const ChatResponse& response) const;

private:
    struct ProviderLimit {
        const ChatCompletionProvider* provider;
        std::size_t active = 0;
    };

    std::map<std::string, std::shared_ptr<ProviderLimit>> providers_;
    std::size_t max_concurrent_per_provider_;
};

} // namespace ailinux::ai
