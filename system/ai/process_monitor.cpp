#include "process_monitor.hpp"

#include "tool_broker.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>

namespace ailinux::ai {
namespace {

constexpr std::size_t kMaximumProcessEntries = 4096;

struct ParsedStat {
    int id = 0;
    int parent_id = 0;
    std::string name;
    char state = '?';
    std::uint64_t user_ticks = 0;
    std::uint64_t system_ticks = 0;
    std::uint64_t start_ticks = 0;
    std::uint64_t virtual_bytes = 0;
    std::int64_t resident_pages = 0;
};

[[nodiscard]] std::optional<int> numeric_directory(const std::string& name) {
    int result = 0;
    const auto [end, error] = std::from_chars(
        name.data(), name.data() + name.size(), result);
    if (error != std::errc{} || end != name.data() + name.size() || result <= 0) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::string read_stat(const std::filesystem::path& path) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        if (errno == ENOENT || errno == ESRCH) {
            throw std::out_of_range("process exited during inspection");
        }
        if (errno == EACCES || errno == EPERM) {
            throw std::system_error(errno, std::generic_category(),
                                    "process information is inaccessible");
        }
        throw ProcessMonitorError("cannot open procfs process statistics");
    }
    std::string content;
    content.reserve(1024);
    char buffer[1024];
    for (;;) {
        const auto count = read(descriptor, buffer, sizeof(buffer));
        if (count < 0) {
            const auto error = errno;
            close(descriptor);
            throw ProcessMonitorError(
                std::error_code(error, std::generic_category()).message());
        }
        if (count == 0) break;
        if (content.size() + static_cast<std::size_t>(count) > 16 * 1024) {
            close(descriptor);
            throw ProcessMonitorError("procfs process statistics exceed size limit");
        }
        content.append(buffer, static_cast<std::size_t>(count));
    }
    close(descriptor);
    return content;
}

template <typename Number>
[[nodiscard]] Number parse_number(const std::string& value) {
    Number result{};
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw ProcessMonitorError("procfs process statistics contain invalid numbers");
    }
    return result;
}

[[nodiscard]] ParsedStat parse_stat(const std::string& content) {
    const auto open = content.find(" (");
    const auto close = content.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open ||
        close + 2 >= content.size() || content[close + 1] != ' ') {
        throw ProcessMonitorError("procfs process statistics are malformed");
    }

    ParsedStat result;
    result.id = parse_number<int>(content.substr(0, open));
    result.name = content.substr(open + 2, close - open - 2);
    result.state = content[close + 2];
    std::istringstream fields(content.substr(close + 3));
    std::vector<std::string> values;
    for (std::string value; fields >> value;) {
        values.push_back(std::move(value));
    }
    if (result.id <= 0 || result.name.empty() || values.size() <= 20 ||
        result.state == ' ' || result.state == '\n') {
        throw ProcessMonitorError("procfs process statistics are incomplete");
    }
    result.parent_id = parse_number<int>(values[0]);
    result.user_ticks = parse_number<std::uint64_t>(values[10]);
    result.system_ticks = parse_number<std::uint64_t>(values[11]);
    result.start_ticks = parse_number<std::uint64_t>(values[18]);
    result.virtual_bytes = parse_number<std::uint64_t>(values[19]);
    result.resident_pages = parse_number<std::int64_t>(values[20]);
    if (result.parent_id < 0 || result.resident_pages < 0) {
        throw ProcessMonitorError("procfs process statistics contain invalid values");
    }
    return result;
}

[[nodiscard]] ProcessInfo to_process_info(const ParsedStat& stat, long page_size,
                                          std::size_t threads) {
    const auto pages = static_cast<std::uint64_t>(stat.resident_pages);
    if (pages > std::numeric_limits<std::uint64_t>::max() /
                    static_cast<std::uint64_t>(page_size)) {
        throw ProcessMonitorError("process resident memory value overflows");
    }
    return {stat.id,
            stat.parent_id,
            stat.name,
            stat.state,
            stat.user_ticks,
            stat.system_ticks,
            stat.start_ticks,
            stat.virtual_bytes,
            pages * static_cast<std::uint64_t>(page_size),
            threads};
}

[[nodiscard]] std::size_t count_threads(const std::filesystem::path& path) {
    std::error_code error;
    std::size_t count = 0;
    std::filesystem::directory_iterator iterator(path, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            throw std::out_of_range("process exited during inspection");
        }
        if (error == std::errc::permission_denied) {
            throw std::system_error(error, "process thread list is inaccessible");
        }
        throw ProcessMonitorError("cannot enumerate procfs process threads");
    }
    const std::filesystem::directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
        if (error) throw ProcessMonitorError("cannot enumerate procfs process threads");
        if (numeric_directory(iterator->path().filename().string()).has_value()) {
            ++count;
        }
    }
    if (error) throw ProcessMonitorError("cannot enumerate procfs process threads");
    return count;
}

[[nodiscard]] nlohmann::json process_to_json(const ProcessInfo& process) {
    return {{"pid", process.pid},
            {"ppid", process.parent_pid},
            {"name", process.name},
            {"state", std::string(1, process.state)},
            {"user_cpu_ticks", process.user_cpu_ticks},
            {"system_cpu_ticks", process.system_cpu_ticks},
            {"start_ticks", process.start_ticks},
            {"virtual_memory_bytes", process.virtual_memory_bytes},
            {"resident_memory_bytes", process.resident_memory_bytes},
            {"thread_count", process.thread_count}};
}

} // namespace

ProcfsProcessMonitor::ProcfsProcessMonitor(std::string proc_root)
    : proc_root_(std::move(proc_root)), page_size_(sysconf(_SC_PAGESIZE)) {
    if (proc_root_.empty() || proc_root_.front() != '/' || page_size_ <= 0) {
        throw std::invalid_argument("procfs root must be absolute and page size available");
    }
}

ProcessSnapshot ProcfsProcessMonitor::list_processes() const {
    ProcessSnapshot snapshot;
    std::error_code error;
    std::filesystem::directory_iterator iterator(proc_root_, error);
    if (error) throw ProcessMonitorError("cannot enumerate procfs root");
    const std::filesystem::directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
        if (error) throw ProcessMonitorError("cannot enumerate procfs root");
        const auto pid = numeric_directory(iterator->path().filename().string());
        if (!pid.has_value()) continue;
        try {
            const auto stat = parse_stat(read_stat(iterator->path() / "stat"));
            if (stat.id != *pid) {
                throw ProcessMonitorError("procfs process identifier mismatch");
            }
            const auto threads = count_threads(iterator->path() / "task");
            if (snapshot.processes.size() >= kMaximumProcessEntries) {
                throw ProcessMonitorError(
                    "visible process count exceeds inspection limit");
            }
            snapshot.processes.push_back(
                to_process_info(stat, page_size_, threads));
        } catch (const std::out_of_range&) {
            ++snapshot.exited_during_scan;
        } catch (const std::system_error& failure) {
            if (failure.code() == std::errc::permission_denied) {
                ++snapshot.inaccessible;
            } else {
                throw ProcessMonitorError("cannot read procfs process entry");
            }
        }
    }
    if (error) throw ProcessMonitorError("cannot enumerate procfs root");
    std::sort(snapshot.processes.begin(), snapshot.processes.end(),
              [](const auto& left, const auto& right) {
                  return left.pid < right.pid;
              });
    return snapshot;
}

ProcessInfo ProcfsProcessMonitor::inspect(int pid) const {
    if (pid <= 0) throw std::invalid_argument("PID must be positive");
    const auto path = std::filesystem::path(proc_root_) / std::to_string(pid);
    const auto stat = parse_stat(read_stat(path / "stat"));
    if (stat.id != pid) {
        throw ProcessMonitorError("procfs process identifier mismatch");
    }
    return to_process_info(stat, page_size_, count_threads(path / "task"));
}

std::vector<ThreadInfo> ProcfsProcessMonitor::list_threads(int pid) const {
    if (pid <= 0) throw std::invalid_argument("PID must be positive");
    const auto task_path =
        std::filesystem::path(proc_root_) / std::to_string(pid) / "task";
    std::error_code error;
    std::filesystem::directory_iterator iterator(task_path, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            throw std::out_of_range("process exited during thread inspection");
        }
        if (error == std::errc::permission_denied) {
            throw std::system_error(error, "process thread list is inaccessible");
        }
        throw ProcessMonitorError("cannot enumerate procfs process threads");
    }
    const std::filesystem::directory_iterator end;
    std::vector<ThreadInfo> result;
    for (; iterator != end; iterator.increment(error)) {
        if (error) throw ProcessMonitorError("cannot enumerate procfs process threads");
        const auto tid =
            numeric_directory(iterator->path().filename().string());
        if (!tid.has_value()) continue;
        if (result.size() >= kMaximumProcessEntries) {
            throw ProcessMonitorError("thread count exceeds inspection limit");
        }
        try {
            const auto stat = parse_stat(read_stat(iterator->path() / "stat"));
            if (stat.id != *tid) {
                throw ProcessMonitorError("procfs thread identifier mismatch");
            }
            result.push_back({pid, *tid, stat.name, stat.state,
                              stat.user_ticks, stat.system_ticks});
        } catch (const std::out_of_range&) {
            continue;
        }
    }
    if (error) throw ProcessMonitorError("cannot enumerate procfs process threads");
    if (result.empty()) {
        throw std::out_of_range("process has no readable threads");
    }
    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right) {
                  return left.tid < right.tid;
              });
    return result;
}

void register_process_tools(ToolBroker& broker,
                            const ProcfsProcessMonitor& monitor) {
    broker.register_tool(
        {"process.list",
         {{"type", "object"},
          {"properties", nlohmann::json::object()},
          {"additionalProperties", false}},
         ToolRisk::read_only},
        [monitor](const nlohmann::json&) {
            const auto snapshot = monitor.list_processes();
            nlohmann::json processes = nlohmann::json::array();
            for (const auto& process : snapshot.processes) {
                processes.push_back(process_to_json(process));
            }
            return nlohmann::json{
                {"processes", std::move(processes)},
                {"exited_during_scan", snapshot.exited_during_scan},
                {"inaccessible", snapshot.inaccessible}};
        });
    broker.register_tool(
        {"process.inspect",
         {{"type", "object"},
          {"properties", {{"pid", {{"type", "integer"}}}}},
          {"required", nlohmann::json::array({"pid"})},
          {"additionalProperties", false}},
         ToolRisk::read_only},
        [monitor](const nlohmann::json& arguments) {
            const auto pid = arguments["pid"].get<std::int64_t>();
            if (pid <= 0 || pid > std::numeric_limits<int>::max()) {
                throw ToolExecutionError("invalid_pid");
            }
            return process_to_json(monitor.inspect(static_cast<int>(pid)));
        });
    broker.register_tool(
        {"thread.list",
         {{"type", "object"},
          {"properties", {{"pid", {{"type", "integer"}}}}},
          {"required", nlohmann::json::array({"pid"})},
          {"additionalProperties", false}},
         ToolRisk::read_only},
        [monitor](const nlohmann::json& arguments) {
            const auto pid = arguments["pid"].get<std::int64_t>();
            if (pid <= 0 || pid > std::numeric_limits<int>::max()) {
                throw ToolExecutionError("invalid_pid");
            }
            nlohmann::json threads = nlohmann::json::array();
            for (const auto& thread :
                 monitor.list_threads(static_cast<int>(pid))) {
                threads.push_back(
                    {{"pid", thread.pid},
                     {"tid", thread.tid},
                     {"name", thread.name},
                     {"state", std::string(1, thread.state)},
                     {"user_cpu_ticks", thread.user_cpu_ticks},
                     {"system_cpu_ticks", thread.system_cpu_ticks}});
            }
            return threads;
        });
}

} // namespace ailinux::ai
