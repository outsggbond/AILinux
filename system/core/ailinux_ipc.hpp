#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace ailinux::core {

constexpr std::uint32_t kIpcMagic = 0x414C4950U;
constexpr std::uint16_t kIpcVersion = 1;
constexpr std::size_t kIpcPayloadLimit = 4 * 1024 * 1024;

struct PeerIdentity {
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
    std::int32_t pid = 0;
    int pidfd = -1;
};

class IpcError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

nlohmann::json send_ipc_request(const std::string& socket_path,
                                const nlohmann::json& request,
                                std::uint32_t timeout_ms = 10000);

class IpcServer {
public:
    IpcServer(std::string socket_path,
              std::uint32_t allowed_gid,
              std::uint32_t uid = 0);
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    nlohmann::json receive_once(std::uint32_t timeout_ms = 10000,
                                std::uint32_t allowed_gid = 0);

private:
    std::string socket_path_;
    std::uint32_t allowed_gid_;
    std::uint32_t uid_;
    int listener_ = -1;
};

} // namespace ailinux::core
