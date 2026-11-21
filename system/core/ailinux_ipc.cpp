#include "ailinux_ipc.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/syscall.h>
#include <linux/limits.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace ailinux::core {
namespace {

#pragma pack(push, 1)
struct IpcHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t reserved;
    std::uint32_t payload_length;
    std::uint32_t crc32;
};
#pragma pack(pop)

std::uint32_t crc32(const unsigned char* data, std::size_t length) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0x82F63B78U & (-(crc & 1U)));
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

bool exact_write(int fd, const unsigned char* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t count = write(fd, data + written, size - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool exact_read(int fd, unsigned char* data, std::size_t size,
                std::uint32_t timeout_ms) {
    std::size_t read_size = 0;
    while (read_size < size) {
        pollfd descriptor{fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, static_cast<int>(timeout_ms));
        if (ready <= 0) return false;
        const ssize_t count = read(fd, data + read_size, size - read_size);
        if (count > 0) {
            read_size += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

void encode_header(IpcHeader& header,
                   const std::string& payload) {
    header.magic = kIpcMagic;
    header.version = kIpcVersion;
    header.reserved = 0;
    header.payload_length = static_cast<std::uint32_t>(payload.size());
    const auto* bytes = reinterpret_cast<const unsigned char*>(payload.data());
    header.crc32 = crc32(bytes, payload.size());
}

} // namespace

IpcServer::IpcServer(std::string socket_path,
                     std::uint32_t allowed_gid,
                     std::uint32_t uid)
    : socket_path_(std::move(socket_path)),
      allowed_gid_(allowed_gid), uid_(uid) {
    namespace fs = std::filesystem;
    const fs::path path(socket_path_);
    if (!path.has_parent_path()) throw IpcError("IPC socket path must include a directory");
    fs::create_directories(path.parent_path());
    if (chmod(path.parent_path().c_str(), 0750) != 0) {
        throw IpcError("cannot restrict IPC socket directory");
    }
    if (fs::exists(path) || fs::is_symlink(path)) fs::remove(path);

    listener_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener_ < 0) throw IpcError("cannot create IPC socket");

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(address.sun_path)) {
        close(listener_);
        listener_ = -1;
        throw IpcError("IPC socket path is too long");
    }
    std::memcpy(address.sun_path, socket_path_.c_str(), socket_path_.size());
    if (bind(listener_, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0) {
        close(listener_);
        listener_ = -1;
        throw IpcError("cannot bind IPC socket");
    }
    if (chmod(socket_path_.c_str(), 0660) != 0) {
        close(listener_);
        listener_ = -1;
        throw IpcError("cannot restrict IPC socket permissions");
    }
    if (listen(listener_, 16) != 0) {
        close(listener_);
        listener_ = -1;
        throw IpcError("cannot listen on IPC socket");
    }
}

IpcServer::~IpcServer() {
    if (listener_ >= 0) close(listener_);
    std::error_code ignored;
    std::filesystem::remove(socket_path_, ignored);
}

nlohmann::json IpcServer::receive_once(std::uint32_t timeout_ms,
                                       std::uint32_t allowed_gid) {
    if (listener_ < 0) throw IpcError("IPC server is closed");
    pollfd descriptor{listener_, POLLIN, 0};
    const int ready = poll(&descriptor, 1, static_cast<int>(timeout_ms));
    if (ready <= 0) throw IpcError("IPC receive timed out");

    sockaddr_un peer_address{};
    socklen_t peer_length = sizeof(peer_address);
    int connection = accept4(listener_, reinterpret_cast<sockaddr*>(&peer_address),
                             &peer_length, SOCK_CLOEXEC);
    if (connection < 0) throw IpcError("cannot accept IPC connection");

    ucred credentials{};
    socklen_t credential_length = sizeof(credentials);
    if (getsockopt(connection, SOL_SOCKET, SO_PEERCRED,
                   &credentials, &credential_length) != 0) {
        close(connection);
        throw IpcError("ERR_PEER_AUTH_FAILED");
    }
    if (uid_ != 0 && credentials.uid != uid_) {
        close(connection);
        throw IpcError("ERR_PEER_AUTH_FAILED");
    }
    const std::uint32_t required_gid = allowed_gid != 0 ? allowed_gid : allowed_gid_;
    if (required_gid != 0 && credentials.gid != required_gid) {
        close(connection);
        throw IpcError("ERR_PEER_AUTH_FAILED");
    }

    int pidfd = -1;
#ifdef __linux__
    pidfd = syscall(SYS_pidfd_open, credentials.pid, 0);
#endif
    if (pidfd < 0) {
        close(connection);
        throw IpcError("ERR_PEER_AUTH_FAILED");
    }

    IpcHeader header{};
    if (!exact_read(connection, reinterpret_cast<unsigned char*>(&header),
                    sizeof(header), timeout_ms)) {
        close(pidfd);
        close(connection);
        throw IpcError("ERR_IPC_FRAME_INVALID");
    }
    if (header.magic != kIpcMagic || header.version != kIpcVersion ||
        header.reserved != 0 || header.payload_length > kIpcPayloadLimit) {
        close(pidfd);
        close(connection);
        throw IpcError("ERR_IPC_FRAME_INVALID");
    }

    std::string payload(header.payload_length, '\0');
    if (!exact_read(connection, reinterpret_cast<unsigned char*>(payload.data()),
                    payload.size(), timeout_ms)) {
        close(pidfd);
        close(connection);
        throw IpcError("ERR_IPC_FRAME_INVALID");
    }
    const auto actual_crc = crc32(reinterpret_cast<const unsigned char*>(payload.data()),
                                  payload.size());
    if (actual_crc != header.crc32) {
        close(pidfd);
        close(connection);
        throw IpcError("ERR_IPC_FRAME_INVALID");
    }

    nlohmann::json response;
    try {
        response = nlohmann::json::parse(payload);
    } catch (const nlohmann::json::exception&) {
        close(pidfd);
        close(connection);
        throw IpcError("ERR_IPC_FRAME_INVALID");
    }
    response["_peer"] = {{"uid", credentials.uid},
                         {"gid", credentials.gid},
                         {"pid", credentials.pid},
                         {"pidfd_locked", true}};

    const std::string response_payload = response.dump();
    IpcHeader response_header{};
    encode_header(response_header, response_payload);
    const bool sent = exact_write(connection,
                                  reinterpret_cast<const unsigned char*>(&response_header),
                                  sizeof(response_header)) &&
                      exact_write(connection,
                                  reinterpret_cast<const unsigned char*>(response_payload.data()),
                                  response_payload.size());
    close(pidfd);
    close(connection);
    if (!sent) throw IpcError("ERR_IPC_FRAME_INVALID");
    return response;
}

nlohmann::json send_ipc_request(const std::string& socket_path,
                                const nlohmann::json& request,
                                std::uint32_t timeout_ms) {
    int connection = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (connection < 0) throw IpcError("cannot create IPC client socket");

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) {
        close(connection);
        throw IpcError("IPC socket path is too long");
    }
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size());
    if (::connect(connection, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) != 0) {
        close(connection);
        throw IpcError("cannot connect to IPC socket");
    }

    const std::string payload = request.dump();
    if (payload.size() > kIpcPayloadLimit) {
        close(connection);
        throw IpcError("ERR_IPC_PAYLOAD_TOO_LARGE");
    }
    IpcHeader header{};
    encode_header(header, payload);
    if (!exact_write(connection, reinterpret_cast<const unsigned char*>(&header),
                     sizeof(header)) ||
        !exact_write(connection, reinterpret_cast<const unsigned char*>(payload.data()),
                     payload.size())) {
        close(connection);
        throw IpcError("ERR_IPC_FRAME_INVALID");
    }

    IpcHeader response_header{};
    if (!exact_read(connection, reinterpret_cast<unsigned char*>(&response_header),
                    sizeof(response_header), timeout_ms)) {
        close(connection);
        throw IpcError("ERR_IPC_FRAME_INVALID");
    }
    if (response_header.magic != kIpcMagic ||
        response_header.version != kIpcVersion ||
        response_header.reserved != 0 ||
        response_header.payload_length > kIpcPayloadLimit) {
        close(connection);
        throw IpcError("ERR_IPC_FRAME_INVALID");
    }
    std::string response_payload(response_header.payload_length, '\0');
    if (!exact_read(connection, reinterpret_cast<unsigned char*>(response_payload.data()),
                    response_payload.size(), timeout_ms)) {
        close(connection);
        throw IpcError("ERR_IPC_FRAME_INVALID");
    }
    if (crc32(reinterpret_cast<const unsigned char*>(response_payload.data()),
              response_payload.size()) != response_header.crc32) {
        close(connection);
        throw IpcError("ERR_IPC_FRAME_INVALID");
    }
    close(connection);
    try {
        return nlohmann::json::parse(response_payload);
    } catch (const nlohmann::json::exception&) {
        throw IpcError("ERR_IPC_FRAME_INVALID");
    }
}

} // namespace ailinux::core
