#include "health_monitor.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

constexpr std::uint32_t kMinimumIntervalMs = 1000;
constexpr std::uint32_t kMaximumIntervalMs = 3600000;

struct Options {
    std::uint32_t interval_ms = 10000;
    bool once = false;
};

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            std::cout << "Usage: ailinux-healthd [--interval-ms N] [--once]\n"
                         "  --interval-ms N  Sampling interval (1000..3600000)\n"
                         "  --once           Emit one snapshot and exit\n";
            std::exit(0);
        }
        if (argument == "--once") {
            result.once = true;
            continue;
        }
        if (argument == "--interval-ms" && index + 1 < argc) {
            const std::string_view value(argv[++index]);
            std::uint32_t interval = 0;
            const auto [end, error] = std::from_chars(
                value.data(), value.data() + value.size(), interval);
            if (error != std::errc{} || end != value.data() + value.size() ||
                interval < kMinimumIntervalMs ||
                interval > kMaximumIntervalMs) {
                throw std::invalid_argument(
                    "--interval-ms must be an integer from 1000 to 3600000");
            }
            result.interval_ms = interval;
            continue;
        }
        throw std::invalid_argument("unsupported or incomplete command-line option");
    }
    return result;
}

void write_line(const std::string& line) {
    std::string output = line + '\n';
    std::size_t written = 0;
    while (written < output.size()) {
        const auto count = write(STDOUT_FILENO, output.data() + written,
                                 output.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            throw std::runtime_error("cannot write health snapshot to standard output");
        }
        written += static_cast<std::size_t>(count);
    }
}

int run_service(const Options& options) {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    if (sigprocmask(SIG_BLOCK, &signals, nullptr) != 0) {
        throw std::runtime_error("cannot block service termination signals");
    }
    const int signal_fd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
    if (signal_fd < 0) {
        throw std::runtime_error("cannot create service signal descriptor");
    }

    ailinux::ai::DeterministicHealthMonitor monitor;
    try {
        while (true) {
            write_line(ailinux::ai::health_snapshot_to_json(monitor.sample()).dump());
            if (options.once) break;

            pollfd descriptor{signal_fd, POLLIN, 0};
            const int ready = poll(&descriptor, 1,
                                   static_cast<int>(options.interval_ms));
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) {
                throw std::runtime_error("health service signal wait failed");
            }
            if (ready > 0) {
                signalfd_siginfo signal_info{};
                const auto bytes = read(signal_fd, &signal_info, sizeof(signal_info));
                if (bytes != static_cast<ssize_t>(sizeof(signal_info))) {
                    throw std::runtime_error("cannot read service termination signal");
                }
                if (signal_info.ssi_signo == SIGTERM ||
                    signal_info.ssi_signo == SIGINT) {
                    break;
                }
                throw std::runtime_error("unexpected signal received by health service");
            }
        }
    } catch (...) {
        close(signal_fd);
        throw;
    }
    close(signal_fd);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run_service(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "ailinux-healthd: " << error.what() << '\n';
        return 1;
    }
}
