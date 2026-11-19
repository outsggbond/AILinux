#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace ailinux::ai {

class ProcessMonitorError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ProcessInfo {
    int pid = 0;
    int parent_pid = 0;
    std::string name;
    char state = '?';
    std::uint64_t user_cpu_ticks = 0;
    std::uint64_t system_cpu_ticks = 0;
    std::uint64_t start_ticks = 0;
    std::uint64_t virtual_memory_bytes = 0;
    std::uint64_t resident_memory_bytes = 0;
    std::size_t thread_count = 0;
};

struct ThreadInfo {
    int pid = 0;
    int tid = 0;
    std::string name;
    char state = '?';
    std::uint64_t user_cpu_ticks = 0;
    std::uint64_t system_cpu_ticks = 0;
};

struct ProcessSnapshot {
    std::vector<ProcessInfo> processes;
    std::size_t exited_during_scan = 0;
    std::size_t inaccessible = 0;
};

class ToolBroker;

class ProcfsProcessMonitor {
public:
    explicit ProcfsProcessMonitor(std::string proc_root = "/proc");

    [[nodiscard]] ProcessSnapshot list_processes() const;
    [[nodiscard]] std::vector<ThreadInfo> list_threads(int pid) const;
    [[nodiscard]] ProcessInfo inspect(int pid) const;

private:
    std::string proc_root_;
    long page_size_;
};

void register_process_tools(ToolBroker& broker,
                            const ProcfsProcessMonitor& monitor);

} // namespace ailinux::ai
