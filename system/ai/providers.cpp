#include "providers.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <future>
#include <mutex>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace ailinux::ai {
namespace {

constexpr std::size_t kMaxCredentialBytes = 4096;
std::once_flag curl_initialization;
std::mutex provider_mutex;
std::condition_variable provider_condition;

struct ResponseBuffer {
    std::string body;
    std::size_t limit;
    bool exceeded = false;
};

struct RequestControl {
    const std::atomic<bool>* cancellation;
    std::optional<std::chrono::steady_clock::time_point> deadline;
};

int report_transfer_progress(void* user_data, curl_off_t, curl_off_t,
                             curl_off_t, curl_off_t) {
    const auto* control = static_cast<const RequestControl*>(user_data);
    return (control->cancellation != nullptr && control->cancellation->load()) ||
                   (control->deadline.has_value() &&
                    std::chrono::steady_clock::now() >= *control->deadline)
               ? 1
               : 0;
}

struct ResponseHeaders {
    std::optional<double> retry_after;
};

std::optional<double> parse_retry_after(const std::string& value) {
    double seconds = 0.0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (error == std::errc{} && end == value.data() + value.size() &&
        std::isfinite(seconds)) {
        return std::clamp(seconds, 0.0, 86400.0);
    }
    const auto timestamp = curl_getdate(value.c_str(), nullptr);
    if (timestamp < 0) {
        return std::nullopt;
    }
    return std::max(0.0, std::difftime(timestamp, std::time(nullptr)));
}

std::size_t read_response_header(char* data, std::size_t size, std::size_t count,
                                 void* context) {
    if (size != 0 && count > static_cast<std::size_t>(-1) / size) {
        return 0;
    }
    const auto bytes = size * count;
    auto* headers = static_cast<ResponseHeaders*>(context);
    std::string line(data, bytes);
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (name == "retry-after") {
            auto value = line.substr(colon + 1);
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first != std::string::npos) {
                value.erase(0, first);
                value.erase(value.find_last_not_of(" \t\r\n") + 1);
                headers->retry_after = parse_retry_after(value);
            }
        }
    }
    return bytes;
}

std::size_t write_response(char* data, std::size_t size, std::size_t count,
                           void* context) {
    auto* response = static_cast<ResponseBuffer*>(context);
    if (size != 0 && count > static_cast<std::size_t>(-1) / size) {
        response->exceeded = true;
        return 0;
    }
    const auto bytes = size * count;
    if (bytes > response->limit - std::min(response->limit, response->body.size())) {
        response->exceeded = true;
        return 0;
    }
    response->body.append(data, bytes);
    return bytes;
}

bool safe_credential_name(const std::string& value) {
    if (value.empty() || value == "." || value == "..") {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-';
    });
}

void validate_base_url(const std::string& url) {
    const auto separator = url.find("://");
    if (separator == std::string::npos) {
        throw ProviderConfigurationError(
            "provider URL must be an absolute HTTPS base URL");
    }
    const auto scheme = url.substr(0, separator);
    auto authority = url.substr(separator + 3);
    const auto path_start = authority.find('/');
    authority = authority.substr(0, path_start);
    if (authority.empty() || authority.find('@') != std::string::npos ||
        authority.find('?') != std::string::npos || authority.find('#') != std::string::npos ||
        url.find('?') != std::string::npos || url.find('#') != std::string::npos ||
        std::any_of(url.begin(), url.end(), [](unsigned char ch) {
            return std::isspace(ch) || ch < 0x20 || ch == '\\';
        })) {
        throw ProviderConfigurationError(
            "provider URL contains unsupported authority or query");
    }
    if (scheme != "https" && scheme != "http") {
        throw ProviderConfigurationError(
            "plain HTTP is permitted only for loopback test endpoints");
    }
    std::string host;
    std::string port;
    if (authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == std::string::npos || end == 1) {
            throw ProviderConfigurationError("provider URL has an invalid host");
        }
        host = authority.substr(1, end - 1);
        in6_addr parsed_ipv6{};
        if (inet_pton(AF_INET6, host.c_str(), &parsed_ipv6) != 1) {
            throw ProviderConfigurationError("provider URL has an invalid IPv6 host");
        }
        if (end + 1 < authority.size()) {
            if (authority[end + 1] != ':') {
                throw ProviderConfigurationError("provider URL has an invalid authority");
            }
            port = authority.substr(end + 2);
        }
    } else {
        const auto colon = authority.find(':');
        if (colon != std::string::npos) {
            if (authority.find(':', colon + 1) != std::string::npos) {
                throw ProviderConfigurationError("IPv6 hosts must be bracketed");
            }
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        } else {
            host = authority;
        }
    }
    if (host.empty()) {
        throw ProviderConfigurationError("provider URL has an invalid host");
    }
    if (!port.empty()) {
        unsigned int port_number = 0;
        const auto [end, error] =
            std::from_chars(port.data(), port.data() + port.size(), port_number);
        if (error != std::errc{} || end != port.data() + port.size() ||
            port_number == 0 || port_number > 65535) {
            throw ProviderConfigurationError("provider URL has an invalid port");
        }
    } else if (authority.back() == ':') {
        throw ProviderConfigurationError("provider URL has an invalid port");
    }
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (scheme == "https") {
        return;
    }
    bool loopback = host == "localhost";
    in_addr ipv4{};
    if (!loopback && inet_pton(AF_INET, host.c_str(), &ipv4) == 1) {
        loopback = (ntohl(ipv4.s_addr) >> 24) == 127;
    }
    in6_addr ipv6{};
    if (!loopback && inet_pton(AF_INET6, host.c_str(), &ipv6) == 1) {
        loopback = IN6_IS_ADDR_LOOPBACK(&ipv6);
    }
    if (!loopback) {
        throw ProviderConfigurationError(
            "plain HTTP is permitted only for loopback test endpoints");
    }
}

std::optional<std::uint64_t> optional_count(const nlohmann::json& usage,
                                            const char* key) {
    if (!usage.is_object() || !usage.contains(key) || !usage[key].is_number_unsigned()) {
        if (usage.is_object() && usage.contains(key) && usage[key].is_number_integer() &&
            usage[key].get<std::int64_t>() >= 0) {
            return static_cast<std::uint64_t>(usage[key].get<std::int64_t>());
        }
        return std::nullopt;
    }
    return usage[key].get<std::uint64_t>();
}

} // namespace

ProviderError::ProviderError(std::string provider_name, std::string error_category,
                             std::string message, long http_status,
                             std::optional<double> retry_delay, bool can_retry)
    : std::runtime_error(provider_name + ": " + message),
      provider(std::move(provider_name)), category(std::move(error_category)),
      status(http_status), retry_after(retry_delay), retryable(can_retry) {}

nlohmann::json ChatMessage::to_json() const {
    static const std::array<std::string, 4> valid_roles = {
        "system", "user", "assistant", "tool"};
    if (std::find(valid_roles.begin(), valid_roles.end(), role) == valid_roles.end()) {
        throw std::invalid_argument("unsupported chat message role");
    }
    if ((role == "system" || role == "user") &&
        (!content.has_value() || content->empty())) {
        throw std::invalid_argument("system and user messages require non-empty content");
    }
    if (role == "tool" && (!tool_call_id.has_value() || tool_call_id->empty())) {
        throw std::invalid_argument("tool messages require a tool_call_id");
    }
    if (role != "tool" && tool_call_id.has_value()) {
        throw std::invalid_argument("tool_call_id is only valid on tool messages");
    }
    if (role != "assistant" && !tool_calls.empty()) {
        throw std::invalid_argument("tool_calls are only valid on assistant messages");
    }
    if (role == "assistant" && !content.has_value() && tool_calls.empty()) {
        throw std::invalid_argument("assistant message requires content or tool calls");
    }

    nlohmann::json result = {{"role", role},
                             {"content", content.has_value()
                                             ? nlohmann::json(*content)
                                             : nlohmann::json(nullptr)}};
    if (tool_call_id.has_value()) {
        result["tool_call_id"] = *tool_call_id;
    }
    if (!tool_calls.empty()) {
        result["tool_calls"] = tool_calls;
    }
    return result;
}

nlohmann::json ChatRequest::to_json() const {
    if (model.empty() || messages.empty()) {
        throw std::invalid_argument("chat request requires a model and messages");
    }
    nlohmann::json serialized_messages = nlohmann::json::array();
    for (const auto& message : messages) {
        serialized_messages.push_back(message.to_json());
    }
    nlohmann::json result = {
        {"model", model}, {"messages", std::move(serialized_messages)}, {"stream", false}};
    if (!tools.empty()) {
        result["tools"] = tools;
    }
    if (tool_choice.has_value()) {
        result["tool_choice"] = *tool_choice;
    }
    if (max_tokens.has_value()) {
        if (*max_tokens == 0) {
            throw std::invalid_argument("max_tokens must be positive");
        }
        result["max_tokens"] = *max_tokens;
    }
    return result;
}

ChatCompletionProvider::ChatCompletionProvider(std::string provider_name,
                                               std::string default_base_url,
                                               std::string default_credential,
                                               ProviderConfig config)
    : provider_name_(std::move(provider_name)),
      default_base_url_(std::move(default_base_url)),
      default_credential_(std::move(default_credential)), config_(std::move(config)) {
    if (config_.model.empty()) {
        throw ProviderConfigurationError("provider model must not be empty");
    }
    if (!std::isfinite(config_.timeout_seconds) || config_.timeout_seconds < 1 ||
        config_.timeout_seconds > 300) {
        throw ProviderConfigurationError(
            "provider timeout must be between 1 and 300 seconds");
    }
    if (config_.max_response_bytes < 1024 ||
        config_.max_response_bytes > 16 * 1024 * 1024) {
        throw ProviderConfigurationError("provider response size limit is out of range");
    }
    if (config_.max_completion_tokens == 0 ||
        config_.max_completion_tokens > 1'000'000) {
        throw ProviderConfigurationError(
            "provider maximum completion tokens are out of range");
    }
    if (config_.max_prompt_bytes == 0 ||
        config_.max_prompt_bytes > 16 * 1024 * 1024 ||
        (config_.input_cost_per_million_tokens_usd.has_value() &&
         (!std::isfinite(*config_.input_cost_per_million_tokens_usd) ||
          *config_.input_cost_per_million_tokens_usd < 0)) ||
        (config_.output_cost_per_million_tokens_usd.has_value() &&
         (!std::isfinite(*config_.output_cost_per_million_tokens_usd) ||
          *config_.output_cost_per_million_tokens_usd < 0))) {
        throw ProviderConfigurationError(
            "provider prompt limit or token prices are invalid");
    }
    if (!config_.credential_name.empty() &&
        !safe_credential_name(config_.credential_name)) {
        throw ProviderConfigurationError("invalid systemd credential name");
    }
    if (config_.base_url.empty()) {
        config_.base_url = default_base_url_;
    }
    while (!config_.base_url.empty() && config_.base_url.back() == '/') {
        config_.base_url.pop_back();
    }
    validate_base_url(config_.base_url);
    std::call_once(curl_initialization, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("cannot initialize HTTP transport");
        }
    });
}

std::string ChatCompletionProvider::read_credential() const {
    const char* directory = std::getenv("CREDENTIALS_DIRECTORY");
    if (directory == nullptr || *directory == '\0') {
        throw ProviderConfigurationError("CREDENTIALS_DIRECTORY is not set");
    }
    const auto& name =
        config_.credential_name.empty() ? default_credential_ : config_.credential_name;
    if (!safe_credential_name(name)) {
        throw ProviderConfigurationError("invalid systemd credential name");
    }
    const auto path = std::filesystem::path(directory) / name;
    const int descriptor =
        open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        throw ProviderConfigurationError("cannot read systemd credential");
    }
    struct stat info {};
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
        (info.st_uid != 0 && info.st_uid != geteuid()) ||
        (info.st_mode & (S_IRWXG | S_IRWXO)) != 0 || info.st_size <= 0 ||
        static_cast<std::size_t>(info.st_size) > kMaxCredentialBytes) {
        close(descriptor);
        throw ProviderConfigurationError(
            "systemd credential has unsafe type, owner, mode, or size");
    }
    std::string value(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto count = read(descriptor, value.data() + offset, value.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(descriptor);
            throw ProviderConfigurationError("cannot read systemd credential");
        }
        offset += static_cast<std::size_t>(count);
    }
    close(descriptor);
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' ||
            value.back() == '\t')) {
        value.pop_back();
    }
    if (value.empty() ||
        std::any_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isspace(ch) || ch == '\0';
        })) {
        throw ProviderConfigurationError("systemd credential is invalid");
    }
    return value;
}

ChatResponse ChatCompletionProvider::complete(
    const ChatRequest& request,
    const std::atomic<bool>* cancellation,
    std::optional<std::chrono::steady_clock::time_point> deadline) const {
    if (cancellation != nullptr && cancellation->load()) {
        throw ProviderCancelledError("provider request was cancelled");
    }
    if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline) {
        throw ProviderTaskTimeoutError("task deadline expired");
    }
    if (request.model != config_.model) {
        throw std::invalid_argument("request model does not match configured provider model");
    }
    auto bounded_request = request;
    if (bounded_request.max_tokens.has_value() &&
        *bounded_request.max_tokens > config_.max_completion_tokens) {
        throw std::invalid_argument(
            "request max_tokens exceeds the configured model limit");
    }
    if (!bounded_request.max_tokens.has_value()) {
        bounded_request.max_tokens = config_.max_completion_tokens;
    }
    const auto payload = bounded_request.to_json();
    auto prompt_payload = payload;
    prompt_payload.erase("model");
    prompt_payload.erase("max_tokens");
    prompt_payload.erase("stream");
    if (prompt_payload.dump().size() > config_.max_prompt_bytes) {
        throw std::invalid_argument(
            "request prompt exceeds the configured byte/token safety ceiling");
    }
    const auto credential = read_credential();
    const auto body = payload.dump();
    CURL* handle = curl_easy_init();
    if (handle == nullptr) {
        throw ProviderError(provider_name_, "transport", "cannot initialize HTTP request");
    }
    struct CurlCleanup {
        CURL* handle;
        curl_slist* headers = nullptr;
        ~CurlCleanup() {
            if (headers != nullptr) {
                curl_slist_free_all(headers);
            }
            curl_easy_cleanup(handle);
        }
    } cleanup{handle};
    auto* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    const auto authorization = "Authorization: Bearer " + credential;
    headers = curl_slist_append(headers, authorization.c_str());
    if (headers == nullptr) {
        throw ProviderError(provider_name_, "transport", "cannot allocate request headers");
    }
    cleanup.headers = headers;

    ResponseBuffer response{"", config_.max_response_bytes};
    ResponseHeaders response_headers;
    const auto url = config_.base_url + "/chat/completions";
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(body.size()));
    long timeout_ms =
        static_cast<long>(std::ceil(config_.timeout_seconds * 1000.0));
    if (deadline.has_value()) {
        const auto remaining_ms = std::ceil(
            std::chrono::duration<double, std::milli>(
                *deadline - std::chrono::steady_clock::now())
                .count());
        if (remaining_ms <= 0.0) {
            throw ProviderTaskTimeoutError("task deadline expired");
        }
        if (remaining_ms < static_cast<double>(timeout_ms)) {
            timeout_ms = static_cast<long>(remaining_ms);
        }
    }
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 0L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, read_response_header);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
    const RequestControl control{cancellation, deadline};
    if (cancellation != nullptr || deadline.has_value()) {
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, report_transfer_progress);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &control);
    }
    const auto result = curl_easy_perform(handle);
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    if (response.exceeded) {
        throw ProviderProtocolError(
            provider_name_ + " response exceeds configured size limit");
    }
    if (result != CURLE_OK) {
        if (cancellation != nullptr && cancellation->load()) {
            throw ProviderCancelledError("provider request was cancelled", true);
        }
        if (deadline.has_value() &&
            std::chrono::steady_clock::now() >= *deadline) {
            throw ProviderTaskTimeoutError("task deadline expired", true);
        }
        const auto timed_out = result == CURLE_OPERATION_TIMEDOUT;
        throw ProviderError(provider_name_, timed_out ? "timeout" : "network_error",
                            timed_out ? "provider request timed out"
                                      : "provider connection failed",
                            status, std::nullopt, true);
    }
    if (deadline.has_value() &&
        std::chrono::steady_clock::now() >= *deadline) {
        throw ProviderTaskTimeoutError("task deadline expired", true);
    }
    if (status < 200 || status >= 300) {
        std::string category = "http_error";
        std::string message = "provider returned an HTTP error";
        bool retryable = status >= 500;
        if (status == 301 || status == 302 || status == 303 || status == 307 ||
            status == 308) {
            category = "redirect_rejected";
            message = "provider redirects are disabled";
            retryable = false;
        } else if (status == 401) {
            category = "authentication";
            message = "provider rejected credentials";
            retryable = false;
        } else if (status == 402) {
            category = "insufficient_balance";
            message = "provider account has insufficient balance";
            retryable = false;
        } else if (status == 422) {
            category = "invalid_parameters";
            message = "provider rejected request parameters";
            retryable = false;
        } else if (status == 429) {
            category = "rate_limit";
            message = "provider rate limit or quota was reached";
            retryable = true;
        } else if (status == 500 || status == 502 || status == 503 ||
                   status == 504) {
            category = "service_unavailable";
            message = "provider service is temporarily unavailable";
            retryable = true;
        } else if (status == 400) {
            category = "invalid_request";
            message = "provider rejected the request";
            retryable = false;
        }
        throw ProviderError(provider_name_, category, message, status,
                            response_headers.retry_after, retryable);
    }
    try {
        return parse_response(nlohmann::json::parse(response.body));
    } catch (const nlohmann::json::exception&) {
        throw ProviderProtocolError(provider_name_ + " returned invalid JSON");
    }
}

ChatResponse ChatCompletionProvider::parse_response(
    const nlohmann::json& payload) const {
    try {
        if (!payload.is_object() || !payload.contains("choices") ||
            !payload["choices"].is_array() || payload["choices"].empty()) {
            throw std::invalid_argument("response has no choice");
        }
        const auto& choice = payload["choices"].at(0);
        const auto& message = choice.at("message");
        if (!message.is_object()) {
            throw std::invalid_argument("response has no assistant message");
        }
        ChatResponse result;
        result.response_id = payload.at("id").get<std::string>();
        result.model = payload.value("model", config_.model);
        if (message.contains("content") && !message["content"].is_null()) {
            result.content = message["content"].get<std::string>();
        }
        if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
            result.finish_reason = choice["finish_reason"].get<std::string>();
        }
        if (payload.contains("usage") && payload["usage"].is_object()) {
            result.prompt_tokens = optional_count(payload["usage"], "prompt_tokens");
            result.completion_tokens =
                optional_count(payload["usage"], "completion_tokens");
        }
        if (message.contains("tool_calls") && !message["tool_calls"].is_null()) {
            if (!message["tool_calls"].is_array()) {
                throw std::invalid_argument("tool_calls is not a list");
            }
            for (const auto& item : message["tool_calls"]) {
                const auto& function = item.at("function");
                if (item.value("type", "function") != "function") {
                    throw std::invalid_argument("unsupported tool call type");
                }
                auto arguments = nlohmann::json::parse(
                    function.at("arguments").get<std::string>());
                if (!arguments.is_object()) {
                    throw std::invalid_argument("tool arguments are not an object");
                }
                result.tool_calls.push_back(
                    {item.at("id").get<std::string>(),
                     function.at("name").get<std::string>(), std::move(arguments)});
            }
        }
        if (result.response_id.empty() || result.model.empty() ||
            (!result.content.has_value() && result.tool_calls.empty())) {
            throw std::invalid_argument("response is missing required fields");
        }
        return result;
    } catch (const std::exception&) {
        throw ProviderProtocolError(
            provider_name_ + " returned an unsupported or malformed response");
    }
}

OpenAIProvider::OpenAIProvider(ProviderConfig config)
    : ChatCompletionProvider("openai", "https://api.openai.com/v1",
                             "openai-api-key", std::move(config)) {}

DeepSeekProvider::DeepSeekProvider(ProviderConfig config)
    : ChatCompletionProvider("deepseek", "https://api.deepseek.com",
                             "deepseek-api-key", std::move(config)) {}

KimiProvider::KimiProvider(ProviderConfig config)
    : ChatCompletionProvider("kimi", "https://api.moonshot.ai/v1",
                             "kimi-api-key", std::move(config)) {}

ProviderPool::ProviderPool(
    std::map<std::string, const ChatCompletionProvider*> providers,
    std::size_t max_concurrent_per_provider)
    : max_concurrent_per_provider_(max_concurrent_per_provider) {
    if (max_concurrent_per_provider_ == 0) {
        throw std::invalid_argument("provider concurrency limit must be positive");
    }
    for (const auto& [name, provider] : providers) {
        if (name.empty() || provider == nullptr) {
            throw std::invalid_argument(
                "provider registry requires a non-empty instance key and adapter");
        }
        providers_.emplace(name, std::make_shared<ProviderLimit>(
                                     ProviderLimit{provider, 0}));
    }
}

ChatResponse ProviderPool::complete(const std::string& provider,
                                    const ChatRequest& request,
                                    const std::atomic<bool>* cancellation,
                                    std::optional<std::chrono::steady_clock::time_point>
                                        deadline) const {
    const auto found = providers_.find(provider);
    if (found == providers_.end()) {
        throw ProviderConfigurationError("provider is not registered: " + provider);
    }
    auto& limit = *found->second;
    {
        std::unique_lock lock(provider_mutex);
        while (limit.active >= max_concurrent_per_provider_) {
            if (cancellation != nullptr && cancellation->load()) {
                throw ProviderCancelledError("provider request was cancelled");
            }
            if (deadline.has_value() &&
                std::chrono::steady_clock::now() >= *deadline) {
                throw ProviderTaskTimeoutError("task deadline expired");
            }
            auto wait = std::chrono::milliseconds(25);
            if (deadline.has_value()) {
                wait = std::min(
                    wait, std::chrono::duration_cast<std::chrono::milliseconds>(
                              *deadline - std::chrono::steady_clock::now()));
                if (wait.count() <= 0) {
                    throw ProviderTaskTimeoutError("task deadline expired");
                }
            }
            provider_condition.wait_for(lock, wait);
        }
        if (cancellation != nullptr && cancellation->load()) {
            throw ProviderCancelledError("provider request was cancelled");
        }
        if (deadline.has_value() &&
            std::chrono::steady_clock::now() >= *deadline) {
            throw ProviderTaskTimeoutError("task deadline expired");
        }
        ++limit.active;
    }
    struct Release {
        ProviderLimit& limit;
        ~Release() {
            std::lock_guard lock(provider_mutex);
            --limit.active;
            provider_condition.notify_one();
        }
    } release{limit};
    return limit.provider->complete(request, cancellation, deadline);
}

std::vector<ChatResponse> ProviderPool::complete_many(
    const std::vector<std::pair<std::string, ChatRequest>>& jobs) const {
    std::vector<std::future<ChatResponse>> futures;
    futures.reserve(jobs.size());
    for (const auto& [name, request] : jobs) {
        futures.push_back(std::async(std::launch::async, [this, name, request] {
            return complete(name, request);
        }));
    }
    std::vector<ChatResponse> results;
    results.reserve(futures.size());
    for (auto& future : futures) {
        results.push_back(future.get());
    }
    return results;
}

double ProviderPool::maximum_cost_usd(const std::string& provider,
                                      const ChatRequest& request) const {
    const auto found = providers_.find(provider);
    if (found == providers_.end()) {
        throw ProviderConfigurationError("provider is not registered: " + provider);
    }
    const auto& config = found->second->provider->config();
    if (!config.input_cost_per_million_tokens_usd.has_value() ||
        !config.output_cost_per_million_tokens_usd.has_value()) {
        throw ProviderConfigurationError(
            "provider token prices must be configured before enforcing a cost budget");
    }
    if (request.model != config.model) {
        throw std::invalid_argument(
            "request model does not match configured provider model");
    }
    auto bounded_request = request;
    if (bounded_request.max_tokens.has_value() &&
        *bounded_request.max_tokens > config.max_completion_tokens) {
        throw std::invalid_argument(
            "request max_tokens exceeds the configured model limit");
    }
    if (!bounded_request.max_tokens.has_value()) {
        bounded_request.max_tokens = config.max_completion_tokens;
    }
    auto prompt_payload = bounded_request.to_json();
    prompt_payload.erase("model");
    prompt_payload.erase("max_tokens");
    prompt_payload.erase("stream");
    const auto prompt_bytes = prompt_payload.dump().size();
    if (prompt_bytes > config.max_prompt_bytes) {
        throw std::invalid_argument(
            "request prompt exceeds the configured byte/token safety ceiling");
    }
    const auto input_cost =
        static_cast<double>(prompt_bytes) *
        *config.input_cost_per_million_tokens_usd / 1'000'000.0;
    const auto output_cost =
        static_cast<double>(*bounded_request.max_tokens) *
        *config.output_cost_per_million_tokens_usd / 1'000'000.0;
    const auto cost = input_cost + output_cost;
    if (!std::isfinite(cost)) {
        throw ProviderConfigurationError(
            "configured Provider prices produce an unrepresentable cost");
    }
    return cost;
}

double ProviderPool::actual_cost_usd(const std::string& provider,
                                     const ChatRequest& request,
                                     const ChatResponse& response) const {
    static_cast<void>(maximum_cost_usd(provider, request));
    const auto found = providers_.find(provider);
    if (found == providers_.end()) {
        throw ProviderConfigurationError("provider is not registered: " + provider);
    }
    if (!response.prompt_tokens.has_value() ||
        !response.completion_tokens.has_value()) {
        return maximum_cost_usd(provider, request);
    }
    const auto& config = found->second->provider->config();
    if (!config.input_cost_per_million_tokens_usd.has_value() ||
        !config.output_cost_per_million_tokens_usd.has_value()) {
        throw ProviderConfigurationError(
            "provider token prices must be configured before enforcing a cost budget");
    }
    const auto input_cost =
        static_cast<double>(*response.prompt_tokens) *
        *config.input_cost_per_million_tokens_usd / 1'000'000.0;
    const auto output_cost =
        static_cast<double>(*response.completion_tokens) *
        *config.output_cost_per_million_tokens_usd / 1'000'000.0;
    const auto cost = input_cost + output_cost;
    if (!std::isfinite(cost)) {
        throw ProviderConfigurationError(
            "configured Provider prices produce an unrepresentable cost");
    }
    return cost;
}

} // namespace ailinux::ai
