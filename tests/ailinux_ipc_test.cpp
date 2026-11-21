#include "ailinux_ipc.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <exception>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

int main() {
    try {
        char socket_template[] = "/tmp/ailinux-ipc-test-XXXXXX";
        char* directory = mkdtemp(socket_template);
        if (directory == nullptr) throw ailinux::core::IpcError("cannot create IPC test directory");
        chmod(directory, 0750);
        const std::string socket_path = std::string(directory) + "/ipc.sock";

        ailinux::core::IpcServer server(socket_path, static_cast<std::uint32_t>(getgid()),
                                        static_cast<std::uint32_t>(geteuid()));
        nlohmann::json received;
        std::exception_ptr failure;
        std::thread server_thread([&] {
            try {
                received = server.receive_once(3000);
            } catch (...) {
                failure = std::current_exception();
            }
        });

        const auto response = ailinux::core::send_ipc_request(
            socket_path,
            {{"method", "health.status"}, {"params", nlohmann::json::object()}},
            3000);
        server_thread.join();
        if (failure) std::rethrow_exception(failure);
        if (response["method"] != "health.status" ||
            !received.contains("_peer") ||
            received["_peer"]["uid"] != static_cast<int>(geteuid()) ||
            received["_peer"]["gid"] != static_cast<int>(getgid()) ||
            received["_peer"]["pidfd_locked"] != true) {
            throw ailinux::core::IpcError("IPC peer identity or response contract mismatch");
        }

        std::filesystem::remove_all(directory);
        std::cout << "AILinux IPC peer-auth regression passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
