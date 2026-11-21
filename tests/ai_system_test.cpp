#include "health_monitor.hpp"
#include "council.hpp"
#include "council_manager.hpp"
#include "process_monitor.hpp"
#include "providers.hpp"
#include "reputation.hpp"
#include "runtime.hpp"
#include "system_tools.hpp"
#include "tool_broker.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace ailinux::ai;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function>
void expect_throw(Function&& function, const char* message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/ailinux-cpp-test-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const auto* path = mkdtemp(buffer.data());
        if (path == nullptr) {
            throw std::runtime_error("cannot create temporary test directory");
        }
        path_ = path;
        if (chmod(path_.c_str(), 0700) != 0) {
            throw std::runtime_error("cannot restrict test directory permissions");
        }
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] const std::string& path() const { return path_; }

private:
    std::string path_;
};

class LocalHttpServer {
public:
    explicit LocalHttpServer(int status = 200, std::string body = {},
                             std::string extra_headers = {},
                             std::size_t expected_requests = 1,
                             std::chrono::milliseconds response_delay = {},
                             bool council_mode = false)
        : status_(status), body_(std::move(body)),
          extra_headers_(std::move(extra_headers)),
          expected_requests_(expected_requests), response_delay_(response_delay),
          council_mode_(council_mode) {
        if (body_.empty()) {
            body_ = R"json({"id":"mock-id","model":"test-model","choices":[{"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call-1","type":"function","function":{"name":"system.get_status","arguments":"{\"detail\":\"short\"}"}}]},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":17,"completion_tokens":4}})json";
        }
        socket_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (socket_ < 0) {
            throw std::runtime_error("cannot create local mock socket");
        }
        int reuse = 1;
        setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(socket_, 8) != 0) {
            close(socket_);
            throw std::runtime_error("cannot bind local mock server");
        }
        socklen_t address_size = sizeof(address);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
            close(socket_);
            throw std::runtime_error("cannot read local mock server port");
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~LocalHttpServer() {
        stopping_.store(true);
        if (socket_ >= 0) {
            shutdown(socket_, SHUT_RDWR);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        for (auto& client : clients_) {
            if (client.joinable()) {
                client.join();
            }
        }
        if (socket_ >= 0) {
            close(socket_);
            socket_ = -1;
        }
    }

    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    }
    [[nodiscard]] std::string authorization() const {
        std::lock_guard lock(mutex_);
        return authorization_;
    }
    [[nodiscard]] std::string request_body() const {
        std::lock_guard lock(mutex_);
        return request_body_;
    }
    [[nodiscard]] std::vector<std::string> request_bodies() const {
        std::lock_guard lock(mutex_);
        return request_bodies_;
    }
    [[nodiscard]] int max_active_requests() const {
        return max_active_requests_.load();
    }
    [[nodiscard]] bool client_disconnected() const {
        return client_disconnected_.load();
    }
    [[nodiscard]] int request_count() const { return request_count_.load(); }

private:
    void serve() {
        std::size_t accepted = 0;
        while (accepted < expected_requests_ && !stopping_.load()) {
            pollfd descriptor{socket_, POLLIN, 0};
            const auto ready = poll(&descriptor, 1, 50);
            if (ready <= 0) {
                continue;
            }
            const int client = accept4(socket_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) {
                if (stopping_.load()) {
                    break;
                }
                continue;
            }
            ++accepted;
            clients_.emplace_back([this, client] { serve_client(client); });
        }
    }

    void serve_client(int client) {
        request_count_.fetch_add(1);
        const auto active = active_requests_.fetch_add(1) + 1;
        auto maximum = max_active_requests_.load();
        while (active > maximum &&
               !max_active_requests_.compare_exchange_weak(maximum, active)) {
        }
        struct ActiveRequestGuard {
            std::atomic<int>& active;
            ~ActiveRequestGuard() { active.fetch_sub(1); }
        } active_guard{active_requests_};
        std::string request;
        std::array<char, 4096> buffer{};
        std::size_t header_end = std::string::npos;
        std::size_t content_length = 0;
        while (request.size() < 1024 * 1024) {
            const auto count = recv(client, buffer.data(), buffer.size(), 0);
            if (count <= 0) {
                close(client);
                return;
            }
            request.append(buffer.data(), static_cast<std::size_t>(count));
            header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const auto length_pos = request.find("Content-Length:");
                if (length_pos != std::string::npos && length_pos < header_end) {
                    content_length = static_cast<std::size_t>(
                        std::stoul(request.substr(length_pos + 15)));
                }
                if (request.size() >= header_end + 4 + content_length) {
                    break;
                }
            }
        }
        std::string received_body;
        {
            std::lock_guard lock(mutex_);
            const auto authorization = request.find("Authorization:");
            if (authorization != std::string::npos) {
                const auto end = request.find("\r\n", authorization);
                authorization_ =
                    request.substr(authorization + 14, end - authorization - 14);
                while (!authorization_.empty() && authorization_.front() == ' ') {
                    authorization_.erase(authorization_.begin());
                }
            }
            request_body_ =
                request.substr(header_end + 4, content_length);
            received_body = request_body_;
            request_bodies_.push_back(received_body);
        }
        const auto response_body =
            council_mode_ ? council_response(received_body) : body_;
        const std::string reason = status_ == 200 ? "OK" : "Unauthorized";
        const auto response =
            "HTTP/1.1 " + std::to_string(status_) + " " + reason +
            "\r\nContent-Type: application/json\r\n" + extra_headers_ +
            "Content-Length: " +
            std::to_string(response_body.size()) +
            "\r\nConnection: close\r\n\r\n" + response_body;
        const auto deadline = std::chrono::steady_clock::now() + response_delay_;
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd peer{client, static_cast<short>(POLLIN | POLLRDHUP), 0};
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            const int wait_ms = static_cast<int>(
                std::clamp<std::int64_t>(remaining.count(), 1, 10));
            const auto ready = poll(&peer, 1, wait_ms);
            if (ready > 0 &&
                (peer.revents & (POLLHUP | POLLRDHUP | POLLERR | POLLNVAL)) != 0) {
                client_disconnected_.store(true);
                close(client);
                return;
            }

            if (ready > 0 && (peer.revents & POLLIN) != 0) {
                char byte = 0;
                if (recv(client, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT) == 0) {
                    client_disconnected_.store(true);
                    close(client);
                    return;
                }
            }
        }
        static_cast<void>(send(client, response.data(), response.size(), MSG_NOSIGNAL));
        close(client);
    }

    static std::string council_response(const std::string& request_body) {
        auto content = R"json({"understanding":"task scope","technical_analysis":"independent inspection","recommendation":"bounded C++ change","risks":"provider failure","implementation_steps":"persist then validate","impact":"AI module","tests":"CTest","rollback_plan":"revert change"})json";
        try {
            const auto request = nlohmann::json::parse(request_body);
            const auto system =
                request.at("messages").at(0).at("content").get<std::string>();
            if (system.find("AILINUX_COUNCIL_CROSS_REVIEW") != std::string::npos) {
                content = R"json({"verdict":"concern","findings":"verify retry and recovery behavior"})json";
            } else if (system.find("AILINUX_COUNCIL_INSPECTOR") !=
                       std::string::npos) {
                content = R"json({"verdict":"verified","report":"execution evidence satisfies the plan and test criteria"})json";
            } else if (system.find("AILINUX_COUNCIL_LEADER_DECISION") !=
                       std::string::npos) {
                content = R"json({"final_plan":"implement bounded coordinator","rationale":"independent evidence and review were considered","rejected_alternatives":"skip provider validation","risk_level":"moderate","execution_plan":"execute only after explicit assignment","test_criteria":"provider-backed phases and persistence pass","rollback_conditions":"revert if validation or audit fails"})json";
            }
        } catch (const std::exception&) {
            return {};
        }
        return nlohmann::json{
                   {"id", "council-mock"},
                   {"model", "mock-model"},
                   {"choices",
                    {{{"message", {{"role", "assistant"}, {"content", content}}},
                      {"finish_reason", "stop"}}}},
                   {"usage", {{"prompt_tokens", 20}, {"completion_tokens", 10}}}}
            .dump();
    }

    int socket_ = -1;
    int port_ = 0;
    int status_;
    std::string body_;
    std::string extra_headers_;
    std::size_t expected_requests_;
    std::chrono::milliseconds response_delay_;
    bool council_mode_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> request_count_{0};
    std::atomic<int> active_requests_{0};
    std::atomic<int> max_active_requests_{0};
    std::atomic<bool> client_disconnected_{false};
    std::thread thread_;
    std::vector<std::thread> clients_;
    mutable std::mutex mutex_;
    std::string authorization_;
    std::string request_body_;
    std::vector<std::string> request_bodies_;
};

std::string evidence(const std::string& input) {
    std::array<unsigned char, 32> bytes{};
    if (EVP_Digest(input.data(), input.size(), bytes.data(), nullptr, EVP_sha256(),
                   nullptr) != 1) {
        throw std::runtime_error("cannot calculate test evidence digest");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(64);
    for (const auto byte : bytes) {
        output.push_back(hex[byte >> 4]);
        output.push_back(hex[byte & 0x0f]);
    }
    return output;
}

void write_file(const std::string& path, const std::string& contents) {
    std::ofstream output(path);
    output << contents;
    if (!output) {
        throw std::runtime_error("cannot write test fixture");
    }
}

ChatRequest text_request(std::string model, std::string text) {
    return {std::move(model),
            {ChatMessage{"user", std::move(text), std::nullopt, {}}},
            {},
            std::nullopt,
            std::nullopt};
}

void test_provider_adapter() {
    TemporaryDirectory temporary;
    const auto credential_dir = temporary.path() + "/credentials";
    std::filesystem::create_directory(credential_dir);
    chmod(credential_dir.c_str(), 0700);
    const auto credential_path = credential_dir + "/provider-key";
    write_file(credential_path, "test-secret-key\n");
    chmod(credential_path.c_str(), 0600);
    setenv("CREDENTIALS_DIRECTORY", credential_dir.c_str(), 1);
    LocalHttpServer server;

    ProviderConfig config{"test-model", "provider-key", server.base_url(), 3, 4096,
                          128};
    OpenAIProvider provider(config);
    auto request = text_request("test-model", "check status");
    const auto response = provider.complete(request);
    check(response.response_id == "mock-id", "provider response ID mismatch");
    check(response.tool_calls.size() == 1, "provider tool call was not parsed");
    check(response.tool_calls.front().name == "system.get_status",
          "provider tool name mismatch");
    check(response.prompt_tokens == 17, "provider usage was not parsed");
    check(server.authorization() == "Bearer test-secret-key",
          "provider did not use the protected credential");
    check(server.request_body().find("\"stream\":false") != std::string::npos,
          "provider did not disable streaming");
    check(nlohmann::json::parse(server.request_body())["max_tokens"] == 128,
          "provider did not enforce its configured completion-token cap");
    request.max_tokens = 129;
    expect_throw<std::invalid_argument>(
        [&] { static_cast<void>(provider.complete(request)); },
        "provider accepted a request above its configured token cap");

    const auto defaults =
        OpenAIProvider(ProviderConfig{"model", {}, {}, 30.0, 2 * 1024 * 1024})
            .config();
    check(defaults.base_url == "https://api.openai.com/v1",
          "OpenAI provider default endpoint changed");
    const auto deepseek_defaults =
        DeepSeekProvider(
            ProviderConfig{"model", {}, {}, 30.0, 2 * 1024 * 1024})
            .config();
    const auto kimi_defaults =
        KimiProvider(ProviderConfig{"model", {}, {}, 30.0, 2 * 1024 * 1024})
            .config();
    check(deepseek_defaults.base_url == "https://api.deepseek.com" &&
              kimi_defaults.base_url == "https://api.moonshot.ai/v1",
          "provider default endpoints changed");
    expect_throw<ProviderConfigurationError>(
        [] {
            OpenAIProvider(
                ProviderConfig{"", "key", {}, 30.0, 2 * 1024 * 1024});
        },
        "empty provider model was accepted");
    expect_throw<ProviderConfigurationError>(
        [] {
            OpenAIProvider(ProviderConfig{"model", "key",
                                          "http://127.attacker.invalid/v1", 30.0,
                                          2 * 1024 * 1024});
        },
        "non-loopback HTTP hostname was accepted");
}

void test_provider_errors_and_concurrency() {
    TemporaryDirectory temporary;
    const auto credential_dir = temporary.path() + "/credentials";
    std::filesystem::create_directory(credential_dir);
    chmod(credential_dir.c_str(), 0700);
    write_file(credential_dir + "/provider-key", "test-secret-key\n");
    chmod((credential_dir + "/provider-key").c_str(), 0600);
    setenv("CREDENTIALS_DIRECTORY", credential_dir.c_str(), 1);
    const auto request = text_request("test-model", "check status");

    {
        LocalHttpServer server(
            401, R"json({"error":"secret-that-must-not-be-logged"})json");
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 3, 4096});
        try {
            static_cast<void>(provider.complete(request));
            throw std::runtime_error("HTTP authentication failure was accepted");
        } catch (const ProviderError& error) {
            check(error.category == "authentication" && error.status == 401,
                  "HTTP authentication failure was misclassified");
            check(std::string(error.what()).find("test-secret-key") == std::string::npos &&
                      std::string(error.what()).find("secret-that-must-not-be-logged") ==
                          std::string::npos,
                  "provider error exposed credential or response text");
        }
    }

    {
        LocalHttpServer server(429, "{}", "Retry-After: 3\r\n");
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 3, 4096});
        try {
            static_cast<void>(provider.complete(request));
            throw std::runtime_error("HTTP rate limit was accepted");
        } catch (const ProviderError& error) {
            check(error.category == "rate_limit" && error.retryable &&
                      error.retry_after == 3.0,
                  "rate-limit retry metadata was not preserved");
        }
    }

    {
        LocalHttpServer server(
            302, "{}", "Location: https://example.invalid/redirected\r\n");
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 3, 4096});
        try {
            static_cast<void>(provider.complete(request));
            throw std::runtime_error("provider redirect was followed or accepted");
        } catch (const ProviderError& error) {
            check(error.category == "redirect_rejected" && error.status == 302,
                  "provider redirect was not rejected explicitly");
        }
    }

    {
        LocalHttpServer server(200, std::string(2048, 'x'));
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 3, 1024});
        expect_throw<ProviderProtocolError>(
            [&] { static_cast<void>(provider.complete(request)); },
            "oversized provider response was accepted");
    }

    {
        LocalHttpServer server(200, {}, {}, 6, std::chrono::milliseconds(30));
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 3, 4096});
        ProviderPool pool({{"openai", &provider}}, 2);
        std::vector<std::pair<std::string, ChatRequest>> jobs;
        for (int index = 0; index < 6; ++index) {
            jobs.emplace_back("openai", request);
        }
        const auto responses = pool.complete_many(jobs);
        check(responses.size() == 6,
              "provider pool did not return every concurrent response");
        check(responses.front().response_id == "mock-id",
              "provider pool returned malformed response data");
        if (server.max_active_requests() <= 1 ||
            server.max_active_requests() > 2) {
            throw std::runtime_error(
                "provider pool concurrency mismatch: " +
                std::to_string(server.max_active_requests()));
        }
    }

    {
        LocalHttpServer server(200, {}, {}, 2, std::chrono::milliseconds(30));
        OpenAIProvider compact(
            {"compact-model", "provider-key", server.base_url(), 3, 4096});
        OpenAIProvider capable(
            {"capable-model", "provider-key", server.base_url(), 3, 4096});
        ProviderPool pool({{"openai:compact-model", &compact},
                           {"openai:capable-model", &capable}},
                          1);
        auto compact_request = text_request("compact-model", "analyze");
        auto capable_request = text_request("capable-model", "review");
        const auto responses = pool.complete_many(
            {{"openai:compact-model", compact_request},
             {"openai:capable-model", capable_request}});
        const auto requests = server.request_bodies();
        check(responses.size() == 2 &&
                  responses[0].response_id == "mock-id" &&
                  responses[1].response_id == "mock-id" && requests.size() == 2 &&
                  std::any_of(requests.begin(), requests.end(), [](const auto& body) {
                      return body.find("\"model\":\"compact-model\"") !=
                             std::string::npos;
                  }) &&
                  std::any_of(requests.begin(), requests.end(), [](const auto& body) {
                      return body.find("\"model\":\"capable-model\"") !=
                             std::string::npos;
                  }),
              "same-provider model instances could not run concurrently");
    }

    const auto link_path = credential_dir + "/credential-link";
    std::filesystem::create_symlink(credential_dir + "/provider-key", link_path);
    OpenAIProvider symlink_provider(
        ProviderConfig{"test-model", "credential-link", "http://localhost/v1",
                       30.0, 2 * 1024 * 1024});
    expect_throw<ProviderConfigurationError>(
        [&] { static_cast<void>(symlink_provider.complete(request)); },
        "provider followed a credential symlink");
}

void test_agent_scheduler_persistence_and_cancellation() {
    TemporaryDirectory temporary;
    const auto credential_dir = temporary.path() + "/credentials";
    std::filesystem::create_directory(credential_dir);
    chmod(credential_dir.c_str(), 0700);
    write_file(credential_dir + "/provider-key", "test-secret-key\n");
    chmod((credential_dir + "/provider-key").c_str(), 0600);
    setenv("CREDENTIALS_DIRECTORY", credential_dir.c_str(), 1);
    AgentReputationStore store(temporary.path() + "/scheduler.sqlite3");
    static_cast<void>(store.register_agent("agent-a", "openai", "test-model", "v1",
                                           "prompt", "tools"));
    const auto request = text_request("ignored-model", "run task");

    {
        LocalHttpServer server(200, {}, {}, 3);
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 3, 4096});
        ProviderPool pool({{"openai", &provider}}, 2);
        AgentScheduler scheduler(store, pool, 2, 1);
        std::vector<std::future<AgentTaskResult>> futures;
        for (int index = 0; index < 3; ++index) {
            futures.push_back(scheduler.submit(
                {"scheduled-" + std::to_string(index), "agent-a",
                 "software-development", 1, request}));
        }
        for (auto& future : futures) {
            const auto result = future.get();
            check(result.status.state == "completed" && result.response.has_value(),
                  "Agent scheduler did not complete a provider task");
            check(result.status.prompt_tokens == 17 &&
                      result.status.completion_tokens == 4,
                  "Provider token usage was not persisted with the task");
        }
        check(scheduler.status("scheduled-0").state == "completed",
              "completed task state was not queryable");
        const auto persisted_tasks = store.list_task_records("agent-a");
        check(persisted_tasks.size() == 3 &&
                  std::all_of(persisted_tasks.begin(), persisted_tasks.end(),
                              [](const auto& task) {
                                  return task.prompt_tokens == 17 &&
                                         task.completion_tokens == 4;
                              }),
              "task history did not retain Provider token usage");
    }

    {
        LocalHttpServer server(401, "{}");
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 3, 4096});
        ProviderPool pool({{"openai", &provider}}, 2);
        AgentScheduler scheduler(store, pool, 1, 1);
        const auto result = scheduler
                                .submit({"failed-task", "agent-a",
                                         "software-development", 1, request})
                                .get();
        check(result.status.state == "failed" &&
                  result.status.error_category == "authentication" &&
                  !result.response.has_value(),
              "provider failure was not stored as a redacted task failure");
        const auto runtime = store.runtime_snapshot("agent-a");
        check(runtime.state == "ERROR" &&
                  runtime.last_error_category == "authentication",
              "Agent runtime snapshot omitted its latest Provider failure");
        check(!store.performance("agent-a").error_rate.has_value(),
              "provider failure was incorrectly counted as an Agent error");
    }

    static_cast<void>(store.record_task_submission(
        "restart-task", "agent-a", "software-development", 1,
        "2026-11-19T10:00:00Z"));
    static_cast<void>(store.transition_task("restart-task", "running",
                                            "2026-11-19T10:00:01Z"));
    {
        LocalHttpServer server;
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 3, 4096});
        ProviderPool pool({{"openai", &provider}}, 1);
        AgentScheduler scheduler(store, pool);
        check(scheduler.status("restart-task").state == "interrupted" &&
                  scheduler.status("restart-task").error_category == "process_restart",
              "scheduler startup did not recover an interrupted task");
    }

    {
        LocalHttpServer server(200, {}, {}, 1, std::chrono::seconds(5));
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 3, 4096});
        ProviderPool pool({{"openai", &provider}}, 1);
        AgentScheduler scheduler(store, pool, 2, 2);
        auto future = scheduler.submit({"cancel-task", "agent-a",
                                        "software-development", 1, request});
        for (int attempt = 0; attempt < 1000; ++attempt) {
            if (scheduler.status("cancel-task").state == "running" &&
                !server.request_body().empty()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(!server.request_body().empty(),
              "cancellation test did not reach the provider request");
        auto waiting_future = scheduler.submit(
            {"cancel-slot-task", "agent-a", "software-development", 1, request});
        for (int attempt = 0; attempt < 1000; ++attempt) {
            if (scheduler.status("cancel-slot-task").state == "running") {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(scheduler.status("cancel-slot-task").state == "running",
              "provider-slot cancellation task did not start");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(scheduler.cancel("cancel-slot-task"),
              "task waiting for a provider slot could not be cancelled");
        check(waiting_future.get().status.state == "cancelled",
              "provider-slot cancellation did not reach a terminal state");
        const auto cancellation_started = std::chrono::steady_clock::now();
        check(scheduler.cancel("cancel-task"),
              "running task could not be cancelled");
        const auto result = future.get();
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (server.client_disconnected()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(result.status.state == "cancelled" && !result.response.has_value(),
              "cancelled task did not persist its terminal state");
        check(std::chrono::steady_clock::now() - cancellation_started <
                  std::chrono::seconds(2),
              "cancellation did not promptly interrupt the active provider request");
        check(server.client_disconnected(),
              "cancelled Provider connection remained open");
    }

    {
        LocalHttpServer server(200, {}, {}, 1, std::chrono::milliseconds(250));
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 3, 4096});
        ProviderPool pool({{"openai", &provider}}, 1);
        AgentScheduler scheduler(store, pool, 1, 1, 1);
        auto active = scheduler.submit(
            {"capacity-active", "agent-a", "software-development", 1, request});
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (scheduler.status("capacity-active").state == "running") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        expect_throw<SchedulerCapacityError>(
            [&] {
                static_cast<void>(scheduler.submit(
                    {"capacity-rejected", "agent-a", "software-development", 1,
                     request}));
            },
            "scheduler exceeded its outstanding-task limit");
        expect_throw<std::out_of_range>(
            [&] { static_cast<void>(scheduler.status("capacity-rejected")); },
            "capacity-rejected task was persisted despite rejection");
        check(active.get().status.state == "completed",
              "scheduler failed to finish work after backpressure");
    }

    {
        LocalHttpServer server(200, {}, {}, 4, std::chrono::milliseconds(250));
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 3, 4096});
        ProviderPool pool({{"openai", &provider}}, 1);
        AgentScheduler scheduler(store, pool, 1, 1);
        auto first = scheduler.submit(
            {"priority-first", "agent-a", "software-development", 1,
             text_request("test-model", "priority-first"), 0});
        for (int attempt = 0; attempt < 1000; ++attempt) {
            if (!server.request_body().empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(!server.request_body().empty(),
              "priority test did not start its blocking request");
        auto low = scheduler.submit(
            {"priority-low", "agent-a", "software-development", 1,
             text_request("test-model", "priority-low"), -1});
        auto high_first = scheduler.submit(
            {"priority-high-first", "agent-a", "software-development", 1,
             text_request("test-model", "priority-high-first"), 10});
        auto high_second = scheduler.submit(
            {"priority-high-second", "agent-a", "software-development", 1,
             text_request("test-model", "priority-high-second"), 10});
        const auto first_result = first.get();
        const auto high_first_result = high_first.get();
        const auto high_second_result = high_second.get();
        const auto low_result = low.get();
        check(first_result.status.state == "completed" &&
                  high_first_result.status.state == "completed" &&
                  high_second_result.status.state == "completed" &&
                  low_result.status.state == "completed",
              ("priority queue task states: " + first_result.status.state + "/" +
               high_first_result.status.state + "/" +
               high_second_result.status.state + "/" + low_result.status.state +
               " errors: " +
               first_result.status.error_category.value_or("-") + "/" +
               high_first_result.status.error_category.value_or("-") + "/" +
               high_second_result.status.error_category.value_or("-") + "/" +
               low_result.status.error_category.value_or("-") +
               " requests=" + std::to_string(server.request_count()))
                  .c_str());
        const auto bodies = server.request_bodies();
        check(bodies.size() == 4 &&
                  nlohmann::json::parse(bodies[0])["messages"][0]["content"] ==
                      "priority-first" &&
                  nlohmann::json::parse(bodies[1])["messages"][0]["content"] ==
                      "priority-high-first" &&
                  nlohmann::json::parse(bodies[2])["messages"][0]["content"] ==
                      "priority-high-second" &&
                  nlohmann::json::parse(bodies[3])["messages"][0]["content"] ==
                      "priority-low",
              "scheduler did not prioritize queued work stably");
        expect_throw<std::invalid_argument>(
            [&] {
                static_cast<void>(scheduler.submit(
                    {"priority-invalid", "agent-a", "software-development", 1,
                     text_request("test-model", "invalid priority"), 1001}));
            },
            "scheduler accepted a priority outside the documented bounds");
        expect_throw<std::out_of_range>(
            [&] { static_cast<void>(scheduler.status("priority-invalid")); },
            "invalid-priority task was persisted");
    }

    {
        LocalHttpServer server(200, {}, {}, 1, std::chrono::milliseconds(250));
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 3, 4096});
        ProviderPool pool({{"openai", &provider}}, 1);
        AgentScheduler scheduler(store, pool, 1, 1);
        auto active = scheduler.submit(
            {"timeout-active", "agent-a", "software-development", 1,
             request, 0, std::nullopt});
        for (int attempt = 0; attempt < 1000; ++attempt) {
            if (!server.request_body().empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(!server.request_body().empty(),
              "queued timeout test did not start its blocking request");
        auto expired = scheduler.submit(
            {"timeout-queued", "agent-a", "software-development", 1, request, 0,
             std::chrono::milliseconds(50)});
        check(active.get().status.state == "completed",
              "task ahead of the timed queued task did not complete");
        const auto timed_out = expired.get();
        check(timed_out.status.state == "failed" &&
                  timed_out.status.error_category == "task_timeout" &&
                  server.request_count() == 1,
              "time spent queued was not included in the task deadline");
    }

    {
        LocalHttpServer server(200, {}, {}, 1, std::chrono::seconds(5));
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 10, 4096});
        ProviderPool pool({{"openai", &provider}}, 1);
        AgentScheduler scheduler(store, pool, 1, 1);
        auto future = scheduler.submit(
            {"timeout-inflight", "agent-a", "software-development", 1, request,
             0, std::chrono::milliseconds(100)});
        for (int attempt = 0; attempt < 1000; ++attempt) {
            if (!server.request_body().empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(!server.request_body().empty(),
              "in-flight timeout test did not reach the Provider request");
        const auto result = future.get();
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (server.client_disconnected()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(result.status.state == "failed" &&
                  result.status.error_category == "task_timeout" &&
                  server.client_disconnected(),
              "task deadline did not interrupt the active HTTP request");
    }

    {
        static_cast<void>(store.register_agent("agent-b", "openai", "test-model", "v1",
                                               "prompt", "tools"));
        store.set_agent_max_concurrent_tasks("agent-a", 1);
        store.set_agent_max_concurrent_tasks("agent-b", 2);
        LocalHttpServer server(200, {}, {}, 3, std::chrono::milliseconds(150));
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 3, 4096});
        ProviderPool pool({{"openai", &provider}}, 3);
        AgentScheduler scheduler(store, pool, 3, 3);
        auto active_a = scheduler.submit(
            {"limit-a-active", "agent-a", "software-development", 1, request});
        for (int attempt = 0; attempt < 1000; ++attempt) {
            if (!server.request_body().empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(!server.request_body().empty(),
              "per-Agent concurrency test did not start its first request");
        auto queued_a = scheduler.submit(
            {"limit-a-queued", "agent-a", "software-development", 1, request});
        auto active_b = scheduler.submit(
            {"limit-b-active", "agent-b", "software-development", 1, request});
        check(active_a.get().status.state == "completed" &&
                  queued_a.get().status.state == "completed" &&
                  active_b.get().status.state == "completed" &&
                  server.max_active_requests() == 2,
              "per-Agent concurrency limits were not independently enforced");
    }

    {
        static_cast<void>(store.register_agent("agent-c", "openai", "test-model", "v1",
                                               "prompt", "tools"));
        store.set_agent_max_provider_requests("agent-c", 1);
        LocalHttpServer server(200, {}, {}, 1);
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 3, 4096});
        ProviderPool pool({{"openai", &provider}}, 1);
        AgentScheduler scheduler(store, pool, 1, 1);
        const auto first = scheduler
                               .submit({"request-quota-first", "agent-c",
                                        "software-development", 1, request})
                               .get();
        const auto over_limit = scheduler
                                    .submit({"request-quota-second", "agent-c",
                                             "software-development", 1, request})
                                    .get();
        const auto updated = store.get_agent("agent-c");
        check(first.status.state == "completed" &&
                  over_limit.status.state == "failed" &&
                  over_limit.status.error_category == "request_limit_exceeded" &&
                  updated.max_provider_requests == 1 &&
                  updated.provider_requests_used == 1 &&
                  server.request_count() == 1,
              "Agent Provider request limit did not block excess network requests");
    }

    {
        static_cast<void>(store.register_agent("agent-d", "openai", "test-model", "v1",
                                               "prompt", "tools"));
        static_cast<void>(store.register_agent("agent-e", "openai", "test-model", "v1",
                                               "prompt", "tools"));
        LocalHttpServer server(200, {}, {}, 2, std::chrono::milliseconds(200));
        OpenAIProvider provider(
            ProviderConfig{"test-model", "provider-key", server.base_url(), 3, 4096});
        ProviderPool pool({{"openai", &provider}}, 1);
        AgentScheduler scheduler(store, pool, 1, 1);
        auto active = scheduler.submit(
            {"runtime-busy", "agent-d", "software-development", 1, request});
        for (int attempt = 0; attempt < 1000; ++attempt) {
            if (!server.request_body().empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(!server.request_body().empty(),
              "Agent runtime-state test did not start its active task");
        auto queued = scheduler.submit(
            {"runtime-waiting", "agent-e", "software-development", 1, request});
        const auto busy = store.runtime_snapshot("agent-d");
        const auto waiting = store.runtime_snapshot("agent-e");
        check(busy.state == "BUSY" && busy.running_tasks == 1 &&
                  waiting.state == "WAITING" && waiting.queued_tasks == 1,
              "Agent runtime state did not reflect running and queued tasks");
        check(active.get().status.state == "completed" &&
                  queued.get().status.state == "completed" &&
                  store.runtime_snapshot("agent-d").state == "READY" &&
                  store.runtime_snapshot("agent-e").state == "READY",
              "Agent runtime state did not return to READY after task completion");
        static_cast<void>(store.record_task_submission(
            "runtime-error", "agent-e", "software-development", 1,
            "runtime-error-submitted"));
        static_cast<void>(store.transition_task(
            "runtime-error", "running", "runtime-error-running"));
        static_cast<void>(store.transition_task(
            "runtime-error", "failed", "runtime-error-failed", "mock_failure"));
        const auto failed = store.runtime_snapshot("agent-e");
        check(failed.state == "ERROR" &&
                  failed.last_error_category == "mock_failure" &&
                  failed.last_error_at == "runtime-error-failed",
              "Agent runtime snapshot did not preserve the latest task error");
        static_cast<void>(store.record_task_submission(
            "runtime-recovered", "agent-e", "software-development", 1,
            "runtime-recovered-submitted"));
        static_cast<void>(store.transition_task(
            "runtime-recovered", "running", "runtime-recovered-running"));
        check(store.runtime_snapshot("agent-e").state == "BUSY" &&
                  store.runtime_snapshot("agent-e").last_error_category ==
                      "mock_failure" &&
                  store.runtime_snapshot("agent-e").last_error_at ==
                      "runtime-error-failed",
              "active work did not supersede ERROR while retaining its evidence");
        static_cast<void>(store.transition_task(
            "runtime-recovered", "completed", "runtime-recovered-completed"));
        check(store.runtime_snapshot("agent-e").state == "READY" &&
                  !store.runtime_snapshot("agent-e").last_error_category.has_value() &&
                  !store.runtime_snapshot("agent-e").last_error_at.has_value(),
              "successful recovery did not clear the latest runtime error");
        store.set_status("agent-e", "offline");
        check(store.runtime_snapshot("agent-e").state == "OFFLINE",
              "Agent runtime snapshot omitted its administratively offline state");
        store.set_status("agent-e", "ready");
    }
}

void test_health_monitor() {
    TemporaryDirectory temporary;
    const auto proc = temporary.path() + "/proc";
    std::filesystem::create_directories(proc + "/net");
    write_file(proc + "/stat", "cpu 100 0 100 800 0 0 0 0 900 0\n");
    write_file(proc + "/meminfo",
               "MemTotal: 1000 kB\nMemAvailable: 100 kB\n"
               "SwapTotal: 0 kB\nSwapFree: 0 kB\nHugePages_Total: 0\n");
    write_file(proc + "/loadavg", "1.0 0.8 0.5 1/100 10\n");
    write_file(proc + "/vmstat", "nr_free_pages 42\noom_kill 0\n");
    write_file(proc + "/net/dev",
               "Inter-| Receive | Transmit\n"
               " face |bytes packets errs drop fifo frame compressed multicast|"
               "bytes packets errs drop fifo colls carrier compressed\n"
               "  eth0: 100 0 0 0 0 0 0 0 200 0 0 0 0 0 0 0\n"
               "    lo: 10 0 0 0 0 0 0 0 20 0 0 0 0 0 0 0\n");
    DeterministicHealthMonitor monitor(proc, temporary.path(),
                                      HealthThresholds{80.0, 90.0});
    const auto snapshot = monitor.sample({{"missing-service", 56789}});
    check(snapshot.metrics.size() == 9, "health monitor returned wrong metric count");
    check(snapshot.metrics.front().level == HealthLevel::unknown,
          "first CPU sample must be unknown");
    check(snapshot.metrics[1].level == HealthLevel::critical,
          "memory threshold was not applied");
    check(snapshot.metrics.back().level == HealthLevel::critical,
          "missing process was not reported");
    check(snapshot.metrics[5].name == "network_receive" &&
              !snapshot.metrics[5].value.has_value() &&
              snapshot.metrics[5].level == HealthLevel::unknown &&
              snapshot.metrics[6].name == "network_transmit" &&
              snapshot.metrics[7].name == "oom_kill" &&
              !snapshot.metrics[7].value.has_value() &&
              snapshot.metrics[7].level == HealthLevel::unknown,
          "first network sample did not establish an unknown baseline");
    check(snapshot.overall() == HealthLevel::critical,
          "overall health level was not aggregated");
    expect_throw<std::invalid_argument>(
        [] { DeterministicHealthMonitor monitor("/proc", "/", {95.0, 90.0}); },
        "unordered health thresholds were accepted");
    expect_throw<std::invalid_argument>(
        [] {
            DeterministicHealthMonitor monitor(
                "/proc", "/", {std::numeric_limits<double>::quiet_NaN(), 90.0});
        },
        "non-finite health threshold was accepted");

    write_file(proc + "/meminfo", "MemTotal: invalid kB\n");
    expect_throw<HealthMonitorError>(
        [&] { static_cast<void>(monitor.sample()); },
        "malformed required procfs memory data was accepted");

    write_file(proc + "/meminfo",
               "MemTotal: 1000 kB\nMemAvailable: 100 kB\n"
               "SwapTotal: 0 kB\nSwapFree: 0 kB\nHugePages_Total: 0\n");
    const auto process_dir = proc + "/456";
    std::filesystem::create_directory(process_dir);
    write_file(process_dir + "/stat", "456 (worker process) Z 1 1 1\n");
    const auto process_snapshot = monitor.sample({{"worker", 456}});
    check(process_snapshot.metrics.back().level == HealthLevel::critical,
          "zombie process was reported as healthy");
    const auto cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    write_file(proc + "/loadavg",
               std::to_string(static_cast<double>(cpu_count) * 2.0) +
                   " 0.8 0.5 1/100 10\n");
    const auto load_snapshot = monitor.sample();
    check(load_snapshot.metrics[4].value.has_value() &&
              *load_snapshot.metrics[4].value >= 199.9,
          "load average above available cores was clamped");

    DeterministicHealthMonitor cpu_monitor(proc, temporary.path(),
                                           HealthThresholds{80.0, 90.0});
    const auto first_cpu = cpu_monitor.sample();
    check(first_cpu.metrics.front().level == HealthLevel::unknown,
          "first CPU sample was not marked unknown");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    write_file(proc + "/stat", "cpu 150 0 150 900 0 0 0 0 950 0\n");
    write_file(proc + "/net/dev",
               "Inter-| Receive | Transmit\n"
               " face |bytes packets errs drop fifo frame compressed multicast|"
               "bytes packets errs drop fifo colls carrier compressed\n"
               "  eth0: 1300 0 0 0 0 0 0 0 800 0 0 0 0 0 0 0\n"
               "    lo: 10 0 0 0 0 0 0 0 20 0 0 0 0 0 0 0\n");
    write_file(proc + "/vmstat", "nr_free_pages 42\noom_kill 2\n");
    const auto second_cpu = cpu_monitor.sample();
    check(second_cpu.metrics.front().value.has_value() &&
              std::abs(*second_cpu.metrics.front().value - 50.0) < 0.01,
          "CPU utilization was not calculated from successive samples");
    check(second_cpu.metrics[5].value.has_value() &&
              *second_cpu.metrics[5].value > 0 &&
              second_cpu.metrics[5].unit == "bytes_per_second" &&
              second_cpu.metrics[6].value.has_value() &&
              *second_cpu.metrics[6].value > 0,
          "network byte rates were not calculated from interface counter deltas");
    check(second_cpu.metrics[7].value == 2.0 &&
              second_cpu.metrics[7].level == HealthLevel::critical,
          "new OOM kills were not reported as critical events");

    write_file(proc + "/net/dev", "eth0: malformed\n");
    expect_throw<HealthMonitorError>(
        [&] { static_cast<void>(cpu_monitor.sample()); },
        "malformed procfs network counters were accepted");
    write_file(proc + "/net/dev",
               "Inter-| Receive | Transmit\n"
               " face |bytes packets errs drop fifo frame compressed multicast|"
               "bytes packets errs drop fifo colls carrier compressed\n"
               "  eth0: 1300 0 0 0 0 0 0 0 800 0 0 0 0 0 0 0\n"
               "    lo: 10 0 0 0 0 0 0 0 20 0 0 0 0 0 0 0\n");
    write_file(proc + "/vmstat", "oom_kill invalid\n");
    expect_throw<HealthMonitorError>(
        [&] { static_cast<void>(cpu_monitor.sample()); },
        "malformed procfs OOM counters were accepted");

    HealthAlertManager alerts(2);
    HealthSnapshot warning{{"2026-11-21T12:00:00Z"},
                           {{"memory", 96.0, "percent", HealthLevel::warning},
                            {"oom_kill", 1.0, "count", HealthLevel::critical}}};
    const auto first_alerts = alerts.evaluate(warning);
    check(first_alerts.size() == 2 && !first_alerts[0].recovery &&
              first_alerts[1].level == HealthLevel::critical,
          "health alert manager did not emit initial warning/critical transitions");
    const auto duplicate_alerts = alerts.evaluate(warning);
    check(duplicate_alerts.empty(),
          "health alert manager repeated an unchanged alert");
    HealthSnapshot recovered{{"2026-11-21T12:01:00Z"},
                             {{"memory", 70.0, "percent", HealthLevel::healthy},
                              {"oom_kill", 0.0, "count", HealthLevel::healthy}}};
    const auto recovery_alerts = alerts.evaluate(recovered);
    check(recovery_alerts.size() == 2 && recovery_alerts[0].recovery &&
              recovery_alerts[0].level == HealthLevel::healthy,
          "health alert manager did not emit recovery transitions");
    expect_throw<std::invalid_argument>(
        [] { static_cast<void>(HealthAlertManager{0}); },
        "zero-sized health alert state was accepted");

    const auto alert_database = temporary.path() + "/health-alerts.sqlite3";
    HealthAlertStore alert_store(alert_database, 3);
    for (const auto& alert : first_alerts) alert_store.record(alert);
    const auto persisted = alert_store.recent(10);
    check(persisted.size() == 2 && persisted[0].metric_name == "oom_kill" &&
              persisted[0].level == HealthLevel::critical &&
              alert_store.schema_version() == 1 &&
              alert_store.database_mode() == 0600,
          "health alert SQLite persistence or private mode was not enforced");

    DeterministicInspectorFallback fallback;
    const auto fallback_report = fallback.evaluate(warning);
    check(fallback_report.overall == HealthLevel::critical &&
              fallback_report.critical_metrics.size() == 1 &&
              fallback_report.critical_metrics[0] == "oom_kill" &&
              fallback_report.warning_metrics.size() == 1,
          "deterministic Inspector fallback did not classify critical/warning metrics");
    const auto fallback_json = inspector_fallback_to_json(fallback_report);
    check(fallback_json["type"] == "deterministic_inspector_fallback" &&
              fallback_json["overall"] == "critical",
          "deterministic Inspector fallback JSON contract was not emitted");
}

void test_reputation_and_task_persistence() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() + "/agents.sqlite3";
    AgentReputationStore store(path);
    const auto agent = store.register_agent("agent-a", "provider-a", "model-a", "v1",
                                            "prompt-1", "tools-1");
    check(agent.status == "ready", "new Agent was not ready");
    check(store.database_mode() == 0600, "reputation database mode is not private");
    check(store.schema_version() == 9, "reputation database schema version mismatch");
    check(agent.max_concurrent_tasks == 2,
          "Agent did not receive the default concurrency limit");
    store.set_agent_max_concurrent_tasks("agent-a", 5);
    check(store.get_agent("agent-a").max_concurrent_tasks == 5,
          "Agent concurrency limit was not persisted");
    store.set_agent_max_provider_requests("agent-a", 9);
    check(store.get_agent("agent-a").max_provider_requests == 9 &&
              store.get_agent("agent-a").provider_requests_used == 0,
          "Agent Provider request limit was not persisted");
    expect_throw<std::invalid_argument>(
        [&] { store.set_agent_max_concurrent_tasks("agent-a", 0); },
        "zero per-Agent concurrency limit was accepted");

    const auto task = store.record_task_submission(
        "persistent-task", "agent-a", "software-development", 1,
        "2026-11-19T10:00:00Z");
    check(task.state == "queued", "new task was not queued");
    static_cast<void>(store.transition_task(
        "persistent-task", "running", "2026-11-19T10:00:01Z"));
    const auto events = store.task_history("persistent-task");
    check(events.size() == 2, "task state events were not persisted");
    const auto argument_digest = evidence(R"json({"detail":"short"})json");
    store.record_tool_audit("tool-request-1", "persistent-task", "agent-a",
                            "system.get_status", argument_digest, "executed",
                            "success", "2026-11-19T10:00:02Z");
    store.record_tool_audit("tool-request-2", "persistent-task", "agent-a",
                            "system.restart_service", argument_digest,
                            "approval-required", "confirmation_required",
                            "2026-11-19T10:00:03Z");
    const auto tool_events = store.tool_audit_history("persistent-task");
    check(tool_events.size() == 2 &&
              tool_events.front().decision == "executed" &&
              tool_events.front().arguments_sha256 == argument_digest &&
              tool_events.back().decision == "approval-required",
          "tool audit records were not persisted in order");
    expect_throw<EvidenceError>(
        [&] {
            store.record_tool_audit("tool-request-invalid", "persistent-task",
                                    "agent-a", "system.get_status", "not-a-digest",
                                    "executed", std::nullopt,
                                    "2026-11-19T10:00:04Z");
        },
        "raw or malformed tool arguments were accepted as an audit digest");
    expect_throw<ReputationError>(
        [&] {
            store.record_tool_audit("tool-request-unknown", "unknown-task",
                                    "agent-a", "system.get_status",
                                    argument_digest, "denied", "not_found",
                                    "2026-11-19T10:00:05Z");
        },
        "audit event for an unknown task was persisted");
    expect_throw<TaskConflictError>(
        [&] {
            static_cast<void>(store.record_task_submission(
                "persistent-task", "agent-a", "software-development", 1,
                "2026-11-19T10:00:02Z"));
        },
        "duplicate task identifier was accepted");

    {
        SchedulerLease lease(store);
        expect_throw<SchedulerAlreadyRunningError>(
            [&] { SchedulerLease duplicate(store); },
            "second scheduler acquired the exclusive process lease");
    }
    const auto interrupted =
        store.recover_incomplete_tasks("2026-11-19T10:01:00Z");
    check(interrupted.size() == 1 && interrupted.front().state == "interrupted",
          "incomplete task was not recovered as interrupted");
}

void test_tool_broker_policy_and_audit() {
    TemporaryDirectory temporary;
    AgentReputationStore store(temporary.path() + "/tool-broker.sqlite3");
    static_cast<void>(store.register_agent("agent-a", "provider-a", "model-a", "v1",
                                           "prompt", "tools"));
    static_cast<void>(store.register_agent("agent-b", "provider-b", "model-b", "v1",
                                           "prompt", "tools"));
    static_cast<void>(store.record_task_submission(
        "tool-task", "agent-a", "software-development", 1,
        "2026-11-19T10:00:00Z"));
    static_cast<void>(store.transition_task("tool-task", "running",
                                            "2026-11-19T10:00:01Z"));

    ToolBroker broker(store);
    const nlohmann::json schema = {
        {"type", "object"},
        {"properties", {{"detail", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"detail"})},
        {"additionalProperties", false}};
    broker.register_tool(
        {"system.get_status", schema, ToolRisk::read_only},
        [](const nlohmann::json& arguments) {
            return nlohmann::json{{"status", arguments["detail"]}};
        });
    broker.register_tool(
        {"system.restart_service", schema, ToolRisk::state_change},
        [](const nlohmann::json&) { return nlohmann::json{{"restarted", true}}; });
    broker.register_tool(
        {"system.install_package", schema, ToolRisk::privileged},
        [](const nlohmann::json&) { return nlohmann::json{{"installed", true}}; });
    broker.register_tool(
        {"system.fail", schema, ToolRisk::read_only},
        [](const nlohmann::json&) -> nlohmann::json {
            throw ToolExecutionError("handler_failure");
        });
    expect_throw<ToolConfigurationError>(
        [&] {
            broker.register_tool(
                {"system.bad_schema",
                 {{"type", "object"}, {"properties", nlohmann::json::object()},
                  {"unknownConstraint", 1}},
                 ToolRisk::read_only},
                [](const nlohmann::json&) { return nlohmann::json::object(); });
        },
        "unsupported tool schema keywords were accepted");

    const nlohmann::json arguments = {{"detail", "short"}};
    ToolAuthorization auth{"read-request", "tool-task", "agent-a",
                           {"system.get_status"}, false};
    const auto read_result =
        broker.invoke(auth, "system.get_status", arguments);
    check(read_result.decision == "executed" && read_result.result.has_value() &&
              (*read_result.result)["status"] == "short",
          "authorized read-only tool did not execute");

    auth.request_id = "unauthorized-request";
    auth.allowed_tools.clear();
    const auto unauthorized =
        broker.invoke(auth, "system.get_status", arguments);
    check(unauthorized.decision == "denied" &&
              unauthorized.outcome_category == "tool_not_authorized",
          "tool without explicit authorization was not denied");

    auth.request_id = "approval-request";
    auth.allowed_tools.insert("system.restart_service");
    const auto approval =
        broker.invoke(auth, "system.restart_service", arguments);
    check(approval.decision == "approval-required" &&
              !approval.result.has_value(),
          "state-changing tool executed without confirmation");
    auth.confirmation_granted = true;
    const auto confirmed =
        broker.invoke(auth, "system.restart_service", arguments);
    check(confirmed.decision == "executed",
          "confirmed state-changing tool did not execute");

    auth.request_id = "privileged-request";
    auth.allowed_tools.insert("system.install_package");
    const auto privileged =
        broker.invoke(auth, "system.install_package", arguments);
    check(privileged.decision == "denied" &&
              privileged.outcome_category == "privileged_operations_disabled",
          "privileged tool was not kept disabled");

    auth.request_id = "invalid-arguments-request";
    auth.allowed_tools.insert("system.get_status");
    const auto invalid = broker.invoke(
        auth, "system.get_status", {{"detail", "short"}, {"extra", true}});
    check(invalid.decision == "denied" &&
              invalid.outcome_category == "invalid_arguments",
          "tool arguments outside the registered schema were accepted");

    auth.request_id = "oversized-arguments-request";
    const auto oversized = broker.invoke(
        auth, "system.get_status",
        {{"detail", std::string(70 * 1024, 'x')}});
    check(oversized.decision == "denied" &&
              oversized.outcome_category == "arguments_out_of_bounds",
          "unbounded tool arguments were accepted");

    auth.request_id = "handler-failure-request";
    auth.allowed_tools.insert("system.fail");
    expect_throw<ToolExecutionError>(
        [&] { static_cast<void>(broker.invoke(auth, "system.fail", arguments)); },
        "tool handler failure was hidden");

    auth.request_id = "wrong-agent-request";
    auth.agent_id = "agent-b";
    auth.allowed_tools = {"system.get_status"};
    const auto wrong_agent = broker.invoke(auth, "system.get_status", arguments);
    check(wrong_agent.decision == "denied" &&
              wrong_agent.outcome_category == "agent_or_task_not_active",
          "task assigned to another Agent was allowed to invoke a tool");

    const auto audit = store.tool_audit_history("tool-task");
    check(audit.size() == 12,
          "tool execution, denial, and failure audit events were incomplete");
    bool found_digest = false;
    bool found_failed = false;
    for (const auto& event : audit) {
        if (event.request_id == "read-request") {
            found_digest = event.arguments_sha256 ==
                           evidence(arguments.dump(-1, ' ', true));
        }
        if (event.request_id == "handler-failure-request" &&
            event.decision == "failed" &&
            event.outcome_category == "handler_failure") {
            found_failed = true;
        }
    }
    check(found_digest && found_failed,
          "audit did not retain the canonical argument digest or failure category");
}

std::string proc_stat(int id, const std::string& name, char state, int parent,
                      std::uint64_t user_ticks, std::uint64_t system_ticks,
                      std::uint64_t start_ticks, std::uint64_t virtual_bytes,
                      std::int64_t resident_pages) {
    std::array<std::string, 21> fields{};
    fields.fill("0");
    fields[0] = std::to_string(parent);
    fields[10] = std::to_string(user_ticks);
    fields[11] = std::to_string(system_ticks);
    fields[18] = std::to_string(start_ticks);
    fields[19] = std::to_string(virtual_bytes);
    fields[20] = std::to_string(resident_pages);
    std::string result =
        std::to_string(id) + " (" + name + ") " + state;
    for (const auto& field : fields) result += " " + field;
    return result + "\n";
}

void test_process_monitor_tools() {
    TemporaryDirectory temporary;
    const auto proc = temporary.path() + "/proc";
    const auto process = proc + "/123";
    const auto task = process + "/task";
    std::filesystem::create_directories(task + "/123");
    std::filesystem::create_directories(task + "/124");
    std::filesystem::create_directories(proc + "/net");
    write_file(proc + "/stat", "cpu 100 0 100 800 0 0 0 0 900 0\n");
    write_file(proc + "/meminfo",
               "MemTotal: 1000 kB\nMemAvailable: 100 kB\n"
               "SwapTotal: 0 kB\nSwapFree: 0 kB\n");
    write_file(proc + "/loadavg", "0.5 0.4 0.3 1/100 10\n");
    write_file(proc + "/vmstat", "oom_kill 0\n");
    write_file(proc + "/net/dev",
               "Inter-| Receive | Transmit\n"
               " face |bytes packets errs drop fifo frame compressed multicast|"
               "bytes packets errs drop fifo colls carrier compressed\n"
               "  eth0: 100 0 0 0 0 0 0 0 200 0 0 0 0 0 0 0\n");
    write_file(process + "/stat",
               proc_stat(123, "process name ) with parens", 'S', 1, 21, 7,
                         100, 8192, 2));
    write_file(task + "/123/stat",
               proc_stat(123, "process name ) with parens", 'S', 1, 21, 7,
                         100, 8192, 2));
    write_file(task + "/124/stat",
               proc_stat(124, "worker thread", 'R', 123, 12, 4, 101, 0, 0));
    ProcfsProcessMonitor monitor(proc);
    const auto snapshot = monitor.list_processes();
    check(snapshot.processes.size() == 1 &&
              snapshot.processes.front().pid == 123 &&
              snapshot.processes.front().parent_pid == 1 &&
              snapshot.processes.front().name == "process name ) with parens" &&
              snapshot.processes.front().thread_count == 2 &&
              snapshot.processes.front().user_cpu_ticks == 21 &&
              snapshot.processes.front().system_cpu_ticks == 7 &&
              snapshot.processes.front().virtual_memory_bytes == 8192 &&
              snapshot.processes.front().resident_memory_bytes ==
                  2 * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE)),
          "procfs process enumeration parsed fields incorrectly");
    const auto threads = monitor.list_threads(123);
    check(threads.size() == 2 && threads[0].pid == 123 &&
              threads[0].tid == 123 && threads[1].pid == 123 &&
              threads[1].tid == 124 &&
              threads[1].name == "worker thread",
          "thread listing confused TIDs with process PIDs");
    check(monitor.inspect(123).thread_count == 2,
          "process inspection did not report thread count");
    expect_throw<std::invalid_argument>(
        [&] { static_cast<void>(monitor.inspect(0)); },
        "non-positive process ID was accepted");
    expect_throw<std::out_of_range>(
        [&] { static_cast<void>(monitor.inspect(999)); },
        "missing process was not reported");

    TemporaryDirectory state;
    AgentReputationStore reputation(state.path() + "/process-tools.sqlite3");
    static_cast<void>(reputation.register_agent("agent-a", "provider-a", "model-a",
                                               "v1", "prompt", "tools"));
    static_cast<void>(reputation.record_task_submission(
        "process-task", "agent-a", "system-diagnosis", 1,
        "2026-11-19T10:00:00Z"));
    static_cast<void>(reputation.transition_task(
        "process-task", "running", "2026-11-19T10:00:01Z"));
    ToolBroker broker(reputation);
    register_process_tools(broker, monitor);
    register_system_tools(broker);
    auto health_monitor = std::make_shared<DeterministicHealthMonitor>(
        proc, temporary.path(), HealthThresholds{80.0, 90.0});
    register_health_tools(broker, health_monitor);
    const ToolAuthorization authorization{
        "process-query", "process-task", "agent-a",
        {"process.list", "process.inspect", "thread.list", "system.get_health",
         "system.get_status"},
        false};
    struct utsname host_identity {};
    check(uname(&host_identity) == 0,
          "cannot inspect host identity for broker test");
    const auto host_cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    check(host_cpu_count > 0, "cannot inspect online CPU count for broker test");
    const auto status_result = broker.invoke(
        authorization, "system.get_status", nlohmann::json::object());
    check(status_result.decision == "executed" &&
              (*status_result.result)["sysname"] == host_identity.sysname &&
              (*status_result.result)["release"] == host_identity.release &&
              (*status_result.result)["version"] == host_identity.version &&
              (*status_result.result)["machine"] == host_identity.machine &&
              (*status_result.result)["online_cpus"] == host_cpu_count &&
              status_result.result->size() == 5,
          "system status tool did not report current kernel identity");
    const auto health_result = broker.invoke(
        authorization, "system.get_health", nlohmann::json::object());
    check(health_result.decision == "executed" &&
              (*health_result.result)["schema_version"] == 1 &&
              (*health_result.result)["metrics"].size() == 8 &&
              (*health_result.result)["metrics"][1]["name"] == "memory",
          "health snapshot tool did not return the shared JSON schema");
    const auto process_list =
        broker.invoke(authorization, "process.list", nlohmann::json::object());
    check(process_list.decision == "executed" &&
              (*process_list.result)["processes"].size() == 1 &&
              (*process_list.result)["processes"][0]["pid"] == 123,
          "read-only process list tool did not return visible processes");
    const auto process_result =
        broker.invoke(authorization, "process.inspect", {{"pid", 123}});
    check(process_result.decision == "executed" &&
              (*process_result.result)["pid"] == 123,
          "read-only process tool was not exposed through the broker");
    const auto thread_result =
        broker.invoke(authorization, "thread.list", {{"pid", 123}});
    check(process_result.decision == "executed" &&
              thread_result.result->size() == 2 &&
              (*thread_result.result)[1]["tid"] == 124,
          "read-only thread tool did not preserve PID/TID identity");

    ProcfsProcessMonitor host_monitor;
    const auto host_process = host_monitor.inspect(static_cast<int>(getpid()));
    check(host_process.pid == getpid() && host_process.thread_count > 0,
          "host procfs inspection did not find the test process");
    const auto host_processes = host_monitor.list_processes();
    check(std::any_of(host_processes.processes.begin(),
                      host_processes.processes.end(),
                      [](const auto& process) { return process.pid == getpid(); }),
          "host procfs process enumeration omitted the test process");
    const auto host_threads = host_monitor.list_threads(static_cast<int>(getpid()));
    const auto current_tid = static_cast<int>(syscall(SYS_gettid));
    check(std::any_of(host_threads.begin(), host_threads.end(),
                      [current_tid](const auto& thread) {
                          return thread.tid == current_tid &&
                                 thread.pid == getpid();
                      }),
          "host procfs did not preserve the current thread TID");
}

void add_review(AgentReputationStore& store, const std::string& task_id,
                const std::string& agent_id, const std::string& domain,
                const std::string& verdict = "correct",
                const std::string& severity = "normal") {
    store.record_assignment(task_id, agent_id, domain, 1);
    store.record_review(task_id, agent_id, "independent-reviewer", verdict,
                        severity, evidence(task_id));
}

void test_reputation_thresholds_and_leader_revocation() {
    TemporaryDirectory temporary;
    AgentReputationStore store(temporary.path() + "/governance.sqlite3");
    static_cast<void>(store.register_agent("agent-a", "provider-a", "model-a",
                                           "v1", "prompt", "tools"));
    for (int index = 0; index < 20; ++index) {
        add_review(store, "review-" + std::to_string(index), "agent-a",
                   "software-development", index < 2 ? "agent-error" : "correct");
    }
    const auto exact_limit = store.performance("agent-a", "software-development");
    check(exact_limit.evaluated_tasks == 20 && exact_limit.error_rate == 0.1,
          "ten-percent performance boundary was calculated incorrectly");
    check(exact_limit.eligible_for_governance,
          "an exact ten-percent error rate should remain eligible");
    check(store.performance("agent-a", "linux-kernel").evaluated_tasks == 0,
          "domain scores borrowed reviews from another domain");
    check(store.elect_domain_leader("software-development").selected_agent_id ==
              "agent-a",
          "qualified Agent was not elected Domain Leader");
    check(store.current_domain_leader("software-development") == "agent-a",
          "elected Domain Leader was not active");

    add_review(store, "review-over-limit", "agent-a", "software-development",
               "agent-error");
    check(!store.performance("agent-a").eligible_for_governance,
          "performance above ten percent remained eligible");
    check(!store.current_domain_leader("software-development").has_value(),
          "ineligible Domain Leader was not revoked");
    expect_throw<EligibilityError>(
        [&] {
            store.record_assignment("high-risk", "agent-a",
                                    "software-development", 2);
        },
        "Agent over the error limit was assigned a high-risk task");
    store.record_assignment("low-risk", "agent-a", "software-development", 1);

    store.record_assignment("critical-task", "agent-a", "cyber-security", 1);
    store.record_review("critical-task", "agent-a", "independent-inspector",
                        "agent-error", "critical-security",
                        evidence("critical-finding"));
    check(store.get_agent("agent-a").status == "suspended",
          "critical security finding did not suspend the Agent");
    expect_throw<EligibilityError>(
        [&] { store.set_status("agent-a", "ready"); },
        "Agent with an unresolved incident was allowed to return to ready");
    store.resolve_security_incident("critical-task", "agent-a");
    store.set_status("agent-a", "ready");
    check(store.get_agent("agent-a").status == "ready",
          "resolved security incident did not allow reactivation");
}

void test_inspector_competence_and_role_separation() {
    TemporaryDirectory temporary;
    AgentReputationStore store(temporary.path() + "/inspectors.sqlite3");
    static_cast<void>(store.register_agent("leader", "openai",
                                           "leader-model", "v1", "prompt",
                                           "tools"));
    static_cast<void>(store.register_agent(
        "inspector", "deepseek", "inspector-model", "v1", "prompt",
        "tools", false, true));
    static_cast<void>(store.register_agent(
        "member", "kimi", "member-model", "v1", "prompt", "tools",
        false, false));

    for (int index = 0; index < 20; ++index) {
        add_review(store, "leader-review-" + std::to_string(index), "leader",
                   "software-development");
        add_review(store, "inspector-review-" + std::to_string(index), "inspector",
                   "software-development");
        const auto classification =
            index < 5 ? "true-positive"
                      : index == 5 ? "false-positive"
                                   : index == 19 ? "false-negative"
                                                 : "true-negative";
        const auto task_id = "inspection-" + std::to_string(index);
        store.record_assignment(task_id, "inspector", "software-development", 1);
        store.record_inspection(task_id, "inspector", "leader", classification,
                                "independent-evaluator", evidence(task_id));
    }

    const auto performance = store.inspector_performance("inspector");
    check(performance.eligible && performance.error_rate == 0.1,
          "qualified Inspector did not meet the ten-percent boundary");
    check(performance.precision.has_value() && performance.recall.has_value() &&
              performance.specificity.has_value(),
          "Inspector confusion-matrix scores were not computed");
    check(store.elect_domain_leader("software-development").selected_agent_id ==
              "leader",
          "eligible leader was not appointed");
    check(store.rotate_inspector("software-development").selected_agent_id ==
              "inspector",
          "eligible Inspector was not rotated in");
    check(store.current_domain_leader("software-development") == "leader" &&
              store.current_inspector("software-development") == "inspector",
          "leader and Inspector roles were not kept distinct");
    expect_throw<EligibilityError>(
        [&] {
            store.record_inspection("leader-review-0", "leader", "inspector",
                                    "true-negative", "independent-evaluator",
                                    evidence("invalid-role"));
        },
        "a Domain Leader was allowed to inspect work");

    const auto council_path = temporary.path() + "/council.sqlite3";
    const CouncilOpinion leader_opinion{
        "understand problem", "inspect subsystem", "use safe plan",
        "no privileged change", "implement and test", "AI module only",
        "CTest", "revert branch"};
    const CouncilOpinion member_opinion{
        "same problem", "check boundary", "validate API", "check migration",
        "add integration test", "no system mutation", "targeted tests",
        "retain prior version"};
    const CouncilDecision decision{
        "implement tested C++ change", "fits scope and guardrails",
        "reject privileged route", "low", "member adds tests",
        "all CTest targets pass", "revert the isolated change"};
    {
        CouncilWorkflow council(store, council_path);
        const auto session = council.create_session(
            "council-session", "task-council-1",
            "Design and test a safe C++ change", "software-development",
            "leader", "inspector", {"member"}, "2026-11-19T10:00:00Z");
        check(session.phase == "intake" &&
                  session.task_id == "task-council-1" &&
                  session.task_summary == "Design and test a safe C++ change" &&
                  council.participants("council-session").size() == 3,
              "Council session roles were not persisted");
        struct stat council_database_stat {};
        check(stat(council_path.c_str(), &council_database_stat) == 0 &&
                  (council_database_stat.st_mode & 0777) == 0600,
              "Council database permissions are not owner-only");
        const auto council_target = temporary.path() + "/council-target";
        const auto council_symlink = temporary.path() + "/council-symlink";
        write_file(council_target, "not a database");
        std::filesystem::create_symlink(council_target, council_symlink);
        expect_throw<CouncilError>(
            [&] { CouncilWorkflow unsafe(store, council_symlink); },
            "Council database followed a symbolic link");
        expect_throw<CouncilError>(
            [&] {
                council.begin_cross_review("council-session",
                                           "2026-11-19T10:00:01Z");
            },
            "Council skipped independent analysis");
        council.begin_analysis("council-session", "2026-11-19T10:00:02Z");
        council.submit_opinion("council-session", "leader", leader_opinion,
                               "2026-11-19T10:00:03Z");
        council.submit_opinion("council-session", "member", member_opinion,
                               "2026-11-19T10:00:04Z");
        expect_throw<CouncilError>(
            [&] {
                council.submit_opinion("council-session", "inspector",
                                       leader_opinion, "2026-11-19T10:00:04Z");
            },
            "Inspector was allowed to submit member analysis");
        council.begin_cross_review("council-session", "2026-11-19T10:00:05Z");
        council.submit_cross_review("council-session", "leader", "member",
                                    "concern", "confirm test scope",
                                    "2026-11-19T10:00:06Z");
        expect_throw<CouncilError>(
            [&] {
                council.begin_leader_decision("council-session",
                                              "2026-11-19T10:00:07Z");
            },
            "Council advanced without a peer review from each participant");
        council.submit_cross_review("council-session", "member", "leader",
                                    "support", "rollback is adequate",
                                    "2026-11-19T10:00:07Z");
        check(council.cross_reviews("council-session").size() == 2,
              "Council cross-review records were not persisted");
        council.begin_leader_decision("council-session",
                                      "2026-11-19T10:00:08Z");
        expect_throw<CouncilError>(
            [&] {
                council.submit_decision("council-session", "member", decision,
                                        "2026-11-19T10:00:09Z");
            },
            "Council member was allowed to override the Leader decision");
        council.submit_decision("council-session", "leader", decision,
                                "2026-11-19T10:00:10Z");
        check(council.get_session("council-session").phase == "execution" &&
                  council.decision("council-session")->risk_level == "low",
              "Council Leader decision was not persisted");
        expect_throw<CouncilError>(
            [&] {
                council.submit_execution_report(
                    "council-session", "inspector", "completed",
                    "inspection is not execution", std::nullopt,
                    "2026-11-19T10:00:11Z");
            },
            "Inspector was allowed to execute the selected plan");
        council.submit_execution_report(
            "council-session", "member", "completed",
            "C++ change and tests completed", std::nullopt,
            "2026-11-19T10:00:12Z");
        council.submit_execution_report(
            "council-session", "leader", "completed",
            "reviewed member output", std::nullopt,
            "2026-11-19T10:00:13Z");
        council.begin_inspection("council-session", "leader",
                                 "2026-11-19T10:00:14Z");
        expect_throw<CouncilError>(
            [&] {
                council.submit_inspection(
                    "council-session", "leader", "verified", "self-review",
                    evidence("council-review"),
                    "2026-11-19T10:00:15Z");
            },
            "Council Leader was allowed to replace independent Inspector");
        council.submit_inspection(
            "council-session", "inspector", "changes-requested",
            "add one migration regression", evidence("council-review-1"),
            "2026-11-19T10:00:16Z");
        check(council.get_session("council-session").phase == "execution",
              "Inspector change request did not return work to execution");
        expect_throw<CouncilError>(
            [&] {
                council.begin_inspection("council-session", "leader",
                                         "2026-11-19T10:00:17Z");
            },
            "Council re-used a stale execution report after requested changes");
        council.submit_execution_report(
            "council-session", "leader", "completed",
            "added migration regression", std::nullopt,
            "2026-11-19T10:00:18Z");
        council.begin_inspection("council-session", "leader",
                                 "2026-11-19T10:00:19Z");
        council.submit_inspection(
            "council-session", "inspector", "verified",
            "tests and scope verified", evidence("council-review-2"),
            "2026-11-19T10:00:20Z");
        check(council.get_session("council-session").phase == "archived" &&
                  council.inspection("council-session")->verdict == "verified" &&
                  council.execution_reports("council-session").size() == 3 &&
                  council.inspection_history("council-session").size() == 2 &&
                  council.inspection_history("council-session")[1]
                          .execution_cycle == 1,
              "verified Council result was not archived");
        expect_throw<CouncilError>(
            [&] {
                council.begin_analysis("council-session",
                                       "2026-11-19T10:00:21Z");
            },
            "archived Council session accepted a new phase transition");
    }
    {
        const auto credential_dir = temporary.path() + "/council-credentials";
        std::filesystem::create_directory(credential_dir);
        chmod(credential_dir.c_str(), 0700);
        for (const auto& name : {"openai-key", "deepseek-key", "kimi-key"}) {
            write_file(credential_dir + "/" + name, "mock-council-key\n");
            chmod((credential_dir + "/" + name).c_str(), 0600);
        }
        setenv("CREDENTIALS_DIRECTORY", credential_dir.c_str(), 1);
        LocalHttpServer server(200, {}, {}, 6, std::chrono::milliseconds(30), true);
        OpenAIProvider leader_provider(
            {"leader-model", "openai-key", server.base_url(), 10, 4096});
        DeepSeekProvider inspector_provider(
            {"inspector-model", "deepseek-key", server.base_url(), 10, 4096});
        KimiProvider member_provider(
            {"member-model", "kimi-key", server.base_url(), 10, 4096});
        ProviderPool providers({{"deepseek", &inspector_provider},
                                {"kimi", &member_provider},
                                {"openai", &leader_provider}},
                               2);
        CouncilWorkflow workflow(store, council_path);
        AgentScheduler scheduler(store, providers, 4, 2);
        static_cast<void>(workflow.create_session(
            "provider-council-session", "provider-council-task",
            "Evaluate an independent multi-model C++ plan",
            "software-development", "leader", "inspector", {"member"},
            "2026-11-19T10:00:30Z"));
        CouncilManager manager(workflow, store, scheduler);
        manager.deliberate("provider-council-session");
        check(workflow.get_session("provider-council-session").phase ==
                      "execution" &&
                  workflow.opinion_records("provider-council-session").size() == 2 &&
                  workflow.cross_reviews("provider-council-session").size() == 2 &&
                  workflow.decision("provider-council-session")->risk_level ==
                      "moderate",
              "Provider-backed Council did not persist analysis, reviews, and Leader decision");
        workflow.submit_execution_report(
            "provider-council-session", "member", "completed",
            "provider-backed test implementation", std::nullopt,
            "2026-11-19T10:00:31Z");
        workflow.submit_execution_report(
            "provider-council-session", "leader", "completed",
            "integrated the member changes and tests", std::nullopt,
            "2026-11-19T10:00:32Z");
        manager.inspect_execution("provider-council-session");
        check(workflow.get_session("provider-council-session").phase == "archived" &&
                  workflow.inspection("provider-council-session")->verdict ==
                      "verified",
              "Provider-backed Inspector did not archive verified execution evidence");
        check(server.request_count() == 6 && server.max_active_requests() >= 2,
              "Council did not concurrently consult distinct model providers");
        const auto scheduler_records = store.list_task_records();
        const auto council_tasks = std::count_if(
            scheduler_records.begin(), scheduler_records.end(),
            [](const auto& task) { return task.task_id.starts_with("council-"); });
        check(council_tasks == 6 &&
                  std::all_of(scheduler_records.begin(), scheduler_records.end(),
                              [](const auto& task) {
                                  return !task.task_id.starts_with("council-") ||
                                         task.state == "completed";
                              }),
              "Council Provider calls were not persisted as completed Agent tasks");
    }
    {
        const auto credential_dir = temporary.path() + "/council-failure-credentials";
        std::filesystem::create_directory(credential_dir);
        chmod(credential_dir.c_str(), 0700);
        for (const auto& name : {"openai-key", "deepseek-key", "kimi-key"}) {
            write_file(credential_dir + "/" + name, "mock-council-key\n");
            chmod((credential_dir + "/" + name).c_str(), 0600);
        }
        setenv("CREDENTIALS_DIRECTORY", credential_dir.c_str(), 1);
        LocalHttpServer server(
            401, R"json({"error":"mock authentication failure"})json", {}, 2);
        OpenAIProvider leader_provider(
            {"leader-model", "openai-key", server.base_url(), 10, 4096});
        DeepSeekProvider inspector_provider(
            {"inspector-model", "deepseek-key", server.base_url(), 10, 4096});
        KimiProvider member_provider(
            {"member-model", "kimi-key", server.base_url(), 10, 4096});
        ProviderPool providers({{"deepseek", &inspector_provider},
                                {"kimi", &member_provider},
                                {"openai", &leader_provider}},
                               2);
        CouncilWorkflow workflow(store, council_path);
        AgentScheduler scheduler(store, providers, 2, 1);
        static_cast<void>(workflow.create_session(
            "provider-failure-session", "provider-failure-task",
            "Provider failure must not create fabricated deliberation",
            "software-development", "leader", "inspector", {"member"},
            "2026-11-19T10:00:40Z"));
        CouncilManager manager(workflow, store, scheduler);
        expect_throw<CouncilManagerError>(
            [&] { manager.deliberate("provider-failure-session"); },
            "Council swallowed a Provider authentication failure");
        check(workflow.get_session("provider-failure-session").phase ==
                      "independent-analysis" &&
                  workflow.opinion_records("provider-failure-session").empty() &&
                  server.request_count() == 2,
              "failed parallel Provider batch advanced or partially fabricated Council analysis");
        const auto tasks = store.list_task_records();
        const auto failed_council_tasks = std::count_if(
            tasks.begin(), tasks.end(), [](const auto& task) {
                return task.task_id.starts_with("council-") &&
                       task.state == "failed" &&
                       task.error_category == "authentication";
            });
        check(failed_council_tasks == 2,
              "failed Council Provider calls were not persisted by AgentScheduler");
    }
    {
        CouncilWorkflow reopened(store, council_path);
        check(reopened.get_session("council-session").phase == "archived" &&
                  reopened.get_session("council-session").task_id ==
                      "task-council-1" &&
                  reopened.opinions("council-session").size() == 2 &&
                  reopened.cross_reviews("council-session").size() == 2 &&
                  reopened.phase_history("council-session").size() == 9 &&
                  reopened.execution_reports("council-session").size() == 3 &&
                  reopened.inspection_history("council-session").size() == 2,
              "Council record did not persist across database reopen");
        static_cast<void>(reopened.create_session(
            "revoked-role-session", "task-council-revoked",
            "Check role revocation", "software-development", "leader",
            "inspector", {"member"}, "2026-11-19T10:01:00Z"));
        reopened.begin_analysis("revoked-role-session",
                                "2026-11-19T10:01:01Z");
        reopened.submit_opinion("revoked-role-session", "leader", leader_opinion,
                                "2026-11-19T10:01:02Z");
        reopened.submit_opinion("revoked-role-session", "member", member_opinion,
                                "2026-11-19T10:01:03Z");
        reopened.begin_cross_review("revoked-role-session",
                                    "2026-11-19T10:01:04Z");
        reopened.submit_cross_review("revoked-role-session", "leader", "member",
                                     "support", "review", "2026-11-19T10:01:05Z");
        reopened.submit_cross_review("revoked-role-session", "member", "leader",
                                     "support", "review", "2026-11-19T10:01:06Z");
        reopened.begin_leader_decision("revoked-role-session",
                                       "2026-11-19T10:01:07Z");
        store.set_status("leader", "offline");
        expect_throw<EligibilityError>(
            [&] {
                reopened.submit_decision("revoked-role-session", "leader",
                                         decision, "2026-11-19T10:01:08Z");
            },
            "Council accepted a decision from a revoked Leader");
    }
}

void test_python_fingerprint_and_schema_migration() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() + "/legacy.sqlite3";
    const auto material =
        nlohmann::json::array({"legacy-provider", "legacy-model", "v1",
                               "prompt-1", "tools-1"})
            .dump(-1, ' ', true);
    const auto digest = evidence(material);
    sqlite3* database = nullptr;
    check(sqlite3_open(path.c_str(), &database) == SQLITE_OK,
          "cannot create legacy SQLite fixture");
    const char* schema = R"sql(
        CREATE TABLE agents(
            agent_id TEXT PRIMARY KEY,provider TEXT NOT NULL,model TEXT NOT NULL,
            model_version TEXT NOT NULL,prompt_version TEXT NOT NULL,
            tool_config_version TEXT NOT NULL,fingerprint TEXT NOT NULL,
            status TEXT NOT NULL,created_at TEXT NOT NULL,updated_at TEXT NOT NULL);
        CREATE TABLE assignments(
            task_id TEXT NOT NULL,agent_id TEXT NOT NULL REFERENCES agents(agent_id),
            domain TEXT NOT NULL,difficulty INTEGER NOT NULL,assigned_at TEXT NOT NULL,
            PRIMARY KEY(task_id,agent_id));
        CREATE TABLE reviews(
            task_id TEXT NOT NULL,agent_id TEXT NOT NULL REFERENCES agents(agent_id),
            domain TEXT NOT NULL,difficulty INTEGER NOT NULL,fingerprint TEXT NOT NULL,
            verdict TEXT NOT NULL,severity TEXT NOT NULL,evaluator_id TEXT NOT NULL,
            evidence_sha256 TEXT NOT NULL,reviewed_at TEXT NOT NULL,resolved_at TEXT,
            PRIMARY KEY(task_id,agent_id));
        PRAGMA user_version=1;
    )sql";
    char* error = nullptr;
    const auto schema_result = sqlite3_exec(database, schema, nullptr, nullptr, &error);
    if (schema_result != SQLITE_OK) {
        const std::string message = error == nullptr ? "cannot create legacy schema" : error;
        sqlite3_free(error);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    sqlite3_stmt* insert = nullptr;
    check(sqlite3_prepare_v2(
              database,
              "INSERT INTO agents VALUES('legacy-agent','legacy-provider',"
              "'legacy-model','v1','prompt-1','tools-1',?,'ready','created','updated')",
              -1, &insert, nullptr) == SQLITE_OK,
          "cannot prepare legacy Agent fixture");
    sqlite3_bind_text(insert, 1, digest.c_str(), -1, SQLITE_TRANSIENT);
    const auto insert_result = sqlite3_step(insert);
    sqlite3_finalize(insert);
    sqlite3_close(database);
    check(insert_result == SQLITE_DONE, "cannot insert legacy Agent fixture");
    chmod(path.c_str(), 0600);

    AgentReputationStore migrated(path);
    check(migrated.schema_version() == 9,
          "legacy reputation schema did not migrate to the current version");
    const auto identity = migrated.register_agent(
        "legacy-agent", "legacy-provider", "legacy-model", "v1", "prompt-1",
        "tools-1");
    check(identity.status == "ready",
          "legacy fingerprint was not compatible with the C++ implementation");
    add_review(migrated, "migrated-review", "legacy-agent",
               "software-development");
    check(migrated.performance("legacy-agent").evaluated_tasks == 1,
          "migrated Agent evidence was not usable after schema upgrade");
}

void test_version_four_tool_audit_migration() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() + "/version-four.sqlite3";
    {
        AgentReputationStore store(path);
        static_cast<void>(store.register_agent("agent-a", "provider-a", "model-a",
                                               "v1", "prompt", "tools"));
        static_cast<void>(store.record_task_submission(
            "migration-task", "agent-a", "software-development", 1,
            "2026-11-19T10:00:00Z"));
    }

    sqlite3* database = nullptr;
    check(sqlite3_open(path.c_str(), &database) == SQLITE_OK,
          "cannot open schema migration fixture");
    char* error = nullptr;
    const auto result =
        sqlite3_exec(database,
                     "DROP TABLE task_cost_reservations;"
                     "DROP TABLE task_provider_requests;"
                     "DROP TABLE agent_cost_budgets;"
                     "ALTER TABLE tasks DROP COLUMN cost_microusd;"
                     "ALTER TABLE agents DROP COLUMN provider_requests_used;"
                     "ALTER TABLE agents DROP COLUMN max_provider_requests;"
                     "ALTER TABLE agents DROP COLUMN max_concurrent_tasks;"
                     "DROP TABLE tool_audit;"
                     "ALTER TABLE tasks DROP COLUMN completion_tokens;"
                     "ALTER TABLE tasks DROP COLUMN prompt_tokens;"
                     "PRAGMA user_version=4;",
                     nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const std::string message = error == nullptr
                                        ? "cannot prepare schema version 4 fixture"
                                        : error;
        sqlite3_free(error);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    sqlite3_close(database);

    AgentReputationStore migrated(path);
    check(migrated.schema_version() == 9,
          "schema version 4 database did not migrate to version 9");
    const auto migrated_task = migrated.get_task_record("migration-task");
    check(!migrated_task.prompt_tokens.has_value() &&
              !migrated_task.completion_tokens.has_value(),
          "schema migration fabricated usage for a historical task");
    migrated.record_tool_audit("migration-request", "migration-task", "agent-a",
                               "system.get_status", evidence("arguments"),
                               "denied", "migration_test",
                               "2026-11-19T10:00:01Z");
    check(migrated.tool_audit_history("migration-task").size() == 1,
          "migrated schema did not accept tool audit records");
}

void test_agent_cost_budgets() {
    TemporaryDirectory temporary;
    AgentReputationStore store(temporary.path() + "/budgets.sqlite3");
    static_cast<void>(store.register_agent("agent-a", "provider-a", "model-a", "v1",
                                           "prompt", "tools"));
    store.set_agent_cost_budget("agent-a", 1.0);
    check(store.cost_budget("agent-a")->limit_usd == 1.0,
          "Agent cost budget was not persisted");
    static_cast<void>(store.record_task_submission(
        "budget-task-a", "agent-a", "software-development", 1, "submitted-a"));
    static_cast<void>(store.record_task_submission(
        "budget-task-b", "agent-a", "software-development", 1, "submitted-b"));
    check(store.reserve_task_cost("budget-task-a", "agent-a", 0.65),
          "configured Agent budget did not reserve task cost");
    check(std::abs(store.cost_budget("agent-a")->reserved_usd - 0.65) < 0.000001,
          "reserved cost was not reflected in the Agent budget");
    expect_throw<CostBudgetExceededError>(
        [&] {
            static_cast<void>(
                store.reserve_task_cost("budget-task-b", "agent-a", 0.4));
        },
        "concurrent reservations exceeded the configured Agent budget");
    store.release_task_cost("budget-task-a");
    check(store.reserve_task_cost("budget-task-b", "agent-a", 0.4),
          "released cost did not become available for another task");
    store.settle_task_cost("budget-task-b", 0.1250001);
    const auto settled = store.get_task_record("budget-task-b");
    check(settled.cost_usd == 0.125001 &&
              store.cost_budget("agent-a")->spent_usd == 0.125001 &&
              store.cost_budget("agent-a")->reserved_usd == 0.0,
          "task cost settlement was not recorded in USD micro-units");
    expect_throw<CostBudgetExceededError>(
        [&] { store.set_agent_cost_budget("agent-a", 0.1); },
        "Agent budget was lowered below already-spent cost");

    static_cast<void>(store.register_agent("agent-b", "provider-b", "model-b", "v1",
                                           "prompt", "tools"));
    store.set_agent_cost_budget("agent-b", 0.000001);
    AgentReputationStore second_connection(temporary.path() + "/budgets.sqlite3");
    static_cast<void>(store.record_task_submission(
        "budget-race-a", "agent-b", "software-development", 1, "submitted-a"));
    static_cast<void>(store.record_task_submission(
        "budget-race-b", "agent-b", "software-development", 1, "submitted-b"));
    std::atomic<int> accepted{0};
    std::atomic<int> winner{-1};
    std::array<std::thread, 2> reservers{
        std::thread([&] {
            try {
                if (store.reserve_task_cost("budget-race-a", "agent-b", 0.000001)) {
                    ++accepted;
                    winner = 0;
                }
            } catch (const CostBudgetExceededError&) {
            }
        }),
        std::thread([&] {
            try {
                if (second_connection.reserve_task_cost(
                        "budget-race-b", "agent-b", 0.000001)) {
                    ++accepted;
                    winner = 1;
                }
            } catch (const CostBudgetExceededError&) {
            }
        })};
    for (auto& reserver : reservers) reserver.join();
    check(accepted == 1,
          "parallel reservations were not atomically bounded by the Agent budget");
    store.release_task_cost(winner == 0 ? "budget-race-a" : "budget-race-b");

    static_cast<void>(store.record_task_submission(
        "budget-recovery", "agent-a", "software-development", 1, "submitted-c"));
    check(store.reserve_task_cost("budget-recovery", "agent-a", 0.3),
          "recovery test did not reserve cost");
    static_cast<void>(
        store.transition_task("budget-recovery", "running", "started"));
    const auto recovered = store.recover_incomplete_tasks("recovered");
    check(std::any_of(recovered.begin(), recovered.end(), [](const auto& task) {
              return task.task_id == "budget-recovery" &&
                     task.state == "interrupted" && task.cost_usd == 0.3;
          }) &&
              store.cost_budget("agent-a")->reserved_usd == 0.0 &&
              std::abs(store.cost_budget("agent-a")->spent_usd - 0.425001) <
                  0.000001,
          "restart recovery did not conservatively settle an active reservation");
}

void test_scheduler_cost_budget_enforcement() {
    TemporaryDirectory temporary;
    const auto credential_dir = temporary.path() + "/credentials";
    std::filesystem::create_directory(credential_dir);
    chmod(credential_dir.c_str(), 0700);
    write_file(credential_dir + "/provider-key", "test-secret-key\n");
    chmod((credential_dir + "/provider-key").c_str(), 0600);
    setenv("CREDENTIALS_DIRECTORY", credential_dir.c_str(), 1);
    AgentReputationStore store(temporary.path() + "/scheduler-budget.sqlite3");
    static_cast<void>(store.register_agent("agent-a", "openai", "test-model", "v1",
                                           "prompt", "tools"));
    store.set_agent_cost_budget("agent-a", 0.00025);
    LocalHttpServer server;
    ProviderConfig configuration{"test-model", "provider-key", server.base_url(),
                                 3, 4096, 128};
    configuration.input_cost_per_million_tokens_usd = 1.0;
    configuration.output_cost_per_million_tokens_usd = 1.0;
    OpenAIProvider provider(configuration);
    ProviderPool pool({{"openai", &provider}}, 1);
    AgentScheduler scheduler(store, pool, 1, 1);
    const auto request = text_request("test-model", "budgeted task");
    const auto completed =
        scheduler.submit({"budgeted-task", "agent-a", "software-development", 1,
                          request})
            .get();
    check(completed.status.state == "completed" &&
              completed.status.cost_usd == 0.000021 &&
              store.cost_budget("agent-a")->spent_usd == 0.000021,
          "scheduler did not settle the Provider usage charge");
    store.set_agent_cost_budget("agent-a", 0.0001);
    const auto rejected =
        scheduler.submit({"over-budget-task", "agent-a", "software-development", 1,
                          request})
            .get();
    check(rejected.status.state == "failed" &&
              rejected.status.error_category == "cost_budget_exceeded" &&
              server.request_count() == 1,
          "over-budget task reached the Provider instead of failing before dispatch");
}

void test_agent_provider_request_quota_atomicity() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() + "/request-quota.sqlite3";
    AgentReputationStore first(path);
    static_cast<void>(first.register_agent("agent-a", "provider-a", "model-a", "v1",
                                           "prompt", "tools"));
    first.set_agent_max_provider_requests("agent-a", 1);
    static_cast<void>(first.record_task_submission(
        "quota-race-a", "agent-a", "software-development", 1, "submitted-a"));
    static_cast<void>(first.record_task_submission(
        "quota-race-b", "agent-a", "software-development", 1, "submitted-b"));
    AgentReputationStore second(path);
    std::atomic<int> accepted{0};
    std::array<std::thread, 2> reservers{
        std::thread([&] {
            if (first.reserve_provider_request("quota-race-a", "agent-a")) {
                ++accepted;
            }
        }),
        std::thread([&] {
            if (second.reserve_provider_request("quota-race-b", "agent-a")) {
                ++accepted;
            }
        })};
    for (auto& reserver : reservers) reserver.join();
    check(accepted == 1 &&
              first.get_agent("agent-a").provider_requests_used == 1,
          "parallel SQLite connections exceeded the Agent request quota");
}

void test_version_seven_concurrency_migration() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() + "/version-seven.sqlite3";
    {
        AgentReputationStore store(path);
        static_cast<void>(store.register_agent("agent-a", "provider-a", "model-a",
                                               "v1", "prompt", "tools"));
    }

    sqlite3* database = nullptr;
    check(sqlite3_open(path.c_str(), &database) == SQLITE_OK,
          "cannot open schema migration fixture");
    char* error = nullptr;
    const auto result =
        sqlite3_exec(database,
                     "DROP TABLE task_provider_requests;"
                     "ALTER TABLE agents DROP COLUMN provider_requests_used;"
                     "ALTER TABLE agents DROP COLUMN max_provider_requests;"
                     "ALTER TABLE agents DROP COLUMN max_concurrent_tasks;"
                     "PRAGMA user_version=7;",
                     nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const std::string message = error == nullptr
                                        ? "cannot prepare schema version 7 fixture"
                                        : error;
        sqlite3_free(error);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    sqlite3_close(database);

    AgentReputationStore migrated(path);
    check(migrated.schema_version() == 9 &&
              migrated.get_agent("agent-a").max_concurrent_tasks == 2 &&
              migrated.get_agent("agent-a").max_provider_requests == 0,
          "schema version 7 database did not migrate with the default limits");
}

void test_version_eight_request_limit_migration() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() + "/version-eight.sqlite3";
    {
        AgentReputationStore store(path);
        static_cast<void>(store.register_agent("agent-a", "provider-a", "model-a",
                                               "v1", "prompt", "tools"));
    }

    sqlite3* database = nullptr;
    check(sqlite3_open(path.c_str(), &database) == SQLITE_OK,
          "cannot open schema migration fixture");
    char* error = nullptr;
    const auto result =
        sqlite3_exec(database,
                     "DROP TABLE task_provider_requests;"
                     "ALTER TABLE agents DROP COLUMN provider_requests_used;"
                     "ALTER TABLE agents DROP COLUMN max_provider_requests;"
                     "PRAGMA user_version=8;",
                     nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const std::string message = error == nullptr
                                        ? "cannot prepare schema version 8 fixture"
                                        : error;
        sqlite3_free(error);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    sqlite3_close(database);

    AgentReputationStore migrated(path);
    check(migrated.schema_version() == 9 &&
              migrated.get_agent("agent-a").max_provider_requests == 0 &&
              migrated.get_agent("agent-a").provider_requests_used == 0,
          "schema version 8 migration did not install unlimited default quota");
}
} // namespace

int main() {
    try {
        test_provider_adapter();
        test_provider_errors_and_concurrency();
        test_agent_scheduler_persistence_and_cancellation();
        test_agent_cost_budgets();
        test_scheduler_cost_budget_enforcement();
        test_agent_provider_request_quota_atomicity();
        test_health_monitor();
        test_reputation_and_task_persistence();
        test_tool_broker_policy_and_audit();
        test_process_monitor_tools();
        test_reputation_thresholds_and_leader_revocation();
        test_inspector_competence_and_role_separation();
        test_python_fingerprint_and_schema_migration();
        test_version_four_tool_audit_migration();
        test_version_seven_concurrency_migration();
        test_version_eight_request_limit_migration();
    } catch (const std::exception& error) {
        std::cerr << "AI system test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "AI system tests passed\n";
    return 0;
}
