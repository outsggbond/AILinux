#include "health_monitor.hpp"
#include "tool_broker.hpp"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <ctime>
#include <fstream>
#include <limits>
#include <memory>
#include <regex>
#include <sstream>

namespace ailinux::ai {
namespace {

constexpr auto kMinimumCpuSampleInterval = std::chrono::milliseconds(100);

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm value{};
    gmtime_r(&seconds, &value);
    std::array<char, 32> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &value) == 0) {
        throw HealthMonitorError("cannot format health sample timestamp");
    }
    return buffer.data();
}

std::string read_file(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw HealthMonitorError("cannot read required health source");
    }
    std::ostringstream content;
    content << input.rdbuf();
    if (input.bad()) {
        throw HealthMonitorError("failed while reading required health source");
    }
    return content.str();
}

std::vector<std::string> split_words(const std::string& line) {
    std::istringstream input(line);
    std::vector<std::string> result;
    for (std::string value; input >> value;) {
        result.push_back(std::move(value));
    }
    return result;
}

} // namespace

HealthLevel HealthSnapshot::overall() const {
    if (metrics.empty()) {
        return HealthLevel::unknown;
    }
    auto result = HealthLevel::healthy;
    for (const auto& metric : metrics) {
        if (metric.level == HealthLevel::critical) {
            return HealthLevel::critical;
        }
        if (metric.level == HealthLevel::warning) {
            result = HealthLevel::warning;
        } else if (metric.level == HealthLevel::unknown &&
                   result == HealthLevel::healthy) {
            result = HealthLevel::unknown;
        }
    }
    return result;
}

nlohmann::json health_snapshot_to_json(const HealthSnapshot& snapshot) {
    nlohmann::json metrics = nlohmann::json::array();
    for (const auto& metric : snapshot.metrics) {
        metrics.push_back({{"name", metric.name},
                           {"value", metric.value.has_value()
                                         ? nlohmann::json(*metric.value)
                                         : nlohmann::json(nullptr)},
                           {"unit", metric.unit},
                           {"level", to_string(metric.level)}});
    }
    return {{"schema_version", 1},
            {"sampled_at", snapshot.sampled_at},
            {"overall", to_string(snapshot.overall())},
            {"metrics", std::move(metrics)}};
}

DeterministicHealthMonitor::DeterministicHealthMonitor(
    std::string proc_root, std::string disk_path, HealthThresholds thresholds)
    : proc_root_(std::move(proc_root)), disk_path_(std::move(disk_path)),
      thresholds_(thresholds) {
    if (!std::isfinite(thresholds_.warning_percent) ||
        !std::isfinite(thresholds_.critical_percent) ||
        thresholds_.warning_percent < 0 ||
        thresholds_.warning_percent >= thresholds_.critical_percent ||
        thresholds_.critical_percent > 100) {
        throw std::invalid_argument(
            "health thresholds must satisfy 0 <= warning < critical <= 100");
    }
}

HealthSnapshot DeterministicHealthMonitor::sample(
    const std::map<std::string, int>& expected_processes) {
    std::lock_guard lock(sample_mutex_);
    HealthSnapshot snapshot{utc_timestamp(), {}};
    snapshot.metrics.push_back(percent_metric("cpu", read_cpu_percent()));
    const auto [memory, swap] = read_memory_percentages();
    snapshot.metrics.push_back(percent_metric("memory", memory));
    snapshot.metrics.push_back(percent_metric("swap", swap));
    snapshot.metrics.push_back(percent_metric("disk", read_disk_percent()));
    snapshot.metrics.push_back(percent_metric("load", read_load_percent()));
    const auto [receive_rate, transmit_rate] = read_network_rates();
    snapshot.metrics.push_back(
        {"network_receive", receive_rate, "bytes_per_second",
         receive_rate.has_value() ? HealthLevel::healthy : HealthLevel::unknown});
    snapshot.metrics.push_back(
        {"network_transmit", transmit_rate, "bytes_per_second",
         transmit_rate.has_value() ? HealthLevel::healthy : HealthLevel::unknown});
    snapshot.metrics.push_back(read_oom_kill_delta());

    static const std::regex safe_label("^[A-Za-z0-9_.-]{1,64}$");
    for (const auto& [name, pid] : expected_processes) {
        if (!std::regex_match(name, safe_label) || pid <= 0) {
            throw std::invalid_argument("expected process entries need safe labels and PIDs");
        }
        const auto path = proc_root_ + "/" + std::to_string(pid) + "/stat";
        const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) {
            if (errno == ENOENT) {
                snapshot.metrics.push_back(
                    {name, 0.0, "running", HealthLevel::critical});
                continue;
            }
            throw HealthMonitorError("cannot read configured process state");
        }
        std::array<char, 4096> buffer{};
        ssize_t count = 0;
        do {
            count = read(descriptor, buffer.data(), buffer.size());
        } while (count < 0 && errno == EINTR);
        close(descriptor);
        if (count < 0) {
            throw HealthMonitorError("cannot read configured process state");
        }
        if (count <= 0) {
            throw HealthMonitorError("configured process state is empty");
        }
        const std::string process_stat(buffer.data(), static_cast<std::size_t>(count));
        const auto end_name = process_stat.rfind(')');
        if (end_name == std::string::npos || end_name + 2 >= process_stat.size()) {
            throw HealthMonitorError("configured process state is malformed");
        }
        const auto state = process_stat[end_name + 2];
        const bool unhealthy = state == 'Z' || state == 'X';
        snapshot.metrics.push_back(
            {name, unhealthy ? 0.0 : 1.0, "running",
             unhealthy ? HealthLevel::critical : HealthLevel::healthy});
    }
    return snapshot;
}

std::pair<std::uint64_t, std::uint64_t>
DeterministicHealthMonitor::read_network_counters() const {
    std::istringstream input(read_file(proc_root_ + "/net/dev"));
    std::string line;
    std::uint64_t received = 0;
    std::uint64_t transmitted = 0;
    std::size_t interfaces = 0;
    while (std::getline(input, line)) {
        const auto separator = line.find(':');
        if (separator == std::string::npos) {
            continue;
        }
        const auto name_start = line.find_first_not_of(" \t");
        const auto name_end =
            separator == 0 ? std::string::npos
                           : line.find_last_not_of(" \t", separator - 1);
        if (name_start == std::string::npos || name_start >= separator ||
            name_end == std::string::npos || name_start > name_end) {
            throw HealthMonitorError("procfs network interface entry is malformed");
        }
        const auto name = line.substr(name_start, name_end - name_start + 1);
        const auto fields = split_words(line.substr(separator + 1));
        if (name.empty() || fields.size() != 16) {
            throw HealthMonitorError("procfs network counters are malformed");
        }
        std::array<std::uint64_t, 16> counters{};
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const auto [end, error] = std::from_chars(
                fields[index].data(), fields[index].data() + fields[index].size(),
                counters[index]);
            if (error != std::errc{} ||
                end != fields[index].data() + fields[index].size()) {
                throw HealthMonitorError("procfs network counters are malformed");
            }
        }
        if (received > std::numeric_limits<std::uint64_t>::max() - counters[0] ||
            transmitted >
                std::numeric_limits<std::uint64_t>::max() - counters[8]) {
            throw HealthMonitorError("procfs network counters overflow");
        }
        received += counters[0];
        transmitted += counters[8];
        ++interfaces;
    }
    if (interfaces == 0) {
        throw HealthMonitorError("procfs network counters contain no interfaces");
    }
    return {received, transmitted};
}

std::pair<std::optional<double>, std::optional<double>>
DeterministicHealthMonitor::read_network_rates() {
    const auto counters = read_network_counters();
    const auto sampled_at = std::chrono::steady_clock::now();
    if (!previous_network_.has_value()) {
        previous_network_ = counters;
        previous_network_sample_time_ = sampled_at;
        return {std::nullopt, std::nullopt};
    }
    const auto interval = sampled_at - previous_network_sample_time_;
    if (interval < kMinimumCpuSampleInterval) {
        return {std::nullopt, std::nullopt};
    }
    const auto [previous_received, previous_transmitted] = *previous_network_;
    if (counters.first < previous_received ||
        counters.second < previous_transmitted) {
        throw HealthMonitorError("procfs network counters moved inconsistently");
    }
    previous_network_ = counters;
    previous_network_sample_time_ = sampled_at;
    const auto seconds = std::chrono::duration<double>(interval).count();
    return {static_cast<double>(counters.first - previous_received) / seconds,
            static_cast<double>(counters.second - previous_transmitted) / seconds};
}

HealthMetric DeterministicHealthMonitor::read_oom_kill_delta() {
    std::istringstream input(read_file(proc_root_ + "/vmstat"));
    std::optional<std::uint64_t> current;
    for (std::string line; std::getline(input, line);) {
        const auto fields = split_words(line);
        if (fields.empty() || fields.front() != "oom_kill") {
            continue;
        }
        std::uint64_t value = 0;
        if (fields.size() != 2) {
            throw HealthMonitorError("procfs OOM counter is malformed");
        }
        const auto [end, error] = std::from_chars(
            fields[1].data(), fields[1].data() + fields[1].size(), value);
        if (error != std::errc{} || end != fields[1].data() + fields[1].size() ||
            current.has_value()) {
            throw HealthMonitorError("procfs OOM counter is malformed");
        }
        current = value;
    }
    if (!current.has_value()) {
        return {"oom_kill", std::nullopt, "count", HealthLevel::unknown};
    }
    if (!previous_oom_kills_.has_value()) {
        previous_oom_kills_ = current;
        return {"oom_kill", std::nullopt, "count", HealthLevel::unknown};
    }
    if (*current < *previous_oom_kills_) {
        throw HealthMonitorError("procfs OOM counter moved inconsistently");
    }
    const auto delta = *current - *previous_oom_kills_;
    previous_oom_kills_ = current;
    return {"oom_kill", static_cast<double>(delta), "count",
            delta > 0 ? HealthLevel::critical : HealthLevel::healthy};
}

std::optional<double> DeterministicHealthMonitor::read_cpu_percent() {
    std::istringstream input(read_file(proc_root_ + "/stat"));
    std::string label;
    std::uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
    std::uint64_t irq = 0, softirq = 0, steal = 0;
    if (!(input >> label >> user >> nice >> system >> idle) || label != "cpu") {
        throw HealthMonitorError("procfs CPU statistics are malformed");
    }
    input >> iowait >> irq >> softirq >> steal;
    const auto total = user + nice + system + idle + iowait + irq + softirq + steal;
    const auto idle_total = idle + iowait;
    const auto sampled_at = std::chrono::steady_clock::now();
    if (!previous_cpu_.has_value()) {
        previous_cpu_ = std::pair{total, idle_total};
        previous_sample_time_ = sampled_at;
        return std::nullopt;
    }
    const auto interval = sampled_at - previous_sample_time_;
    if (interval < kMinimumCpuSampleInterval) {
        return std::nullopt;
    }
    const auto [previous_total, previous_idle] = *previous_cpu_;
    if (total < previous_total || idle_total < previous_idle) {
        throw HealthMonitorError("procfs CPU counters moved inconsistently");
    }
    previous_cpu_ = std::pair{total, idle_total};
    previous_sample_time_ = sampled_at;
    const auto total_delta = total - previous_total;
    const auto idle_delta = idle_total - previous_idle;
    if (total_delta == 0) {
        return std::nullopt;
    }
    return 100.0 * static_cast<double>(total_delta - idle_delta) /
           static_cast<double>(total_delta);
}

std::pair<double, double>
DeterministicHealthMonitor::read_memory_percentages() const {
    std::istringstream input(read_file(proc_root_ + "/meminfo"));
    std::map<std::string, std::uint64_t> values;
    for (std::string line; std::getline(input, line);) {
        std::istringstream fields(line);
        std::string key;
        std::string raw_value;
        std::string unit;
        if (!(fields >> key >> raw_value)) {
            continue;
        }
        if (!key.empty() && key.back() == ':') {
            key.pop_back();
        }
        if (key != "MemTotal" && key != "MemAvailable" && key != "SwapTotal" &&
            key != "SwapFree") {
            continue;
        }
        std::uint64_t value = 0;
        const auto [end, error] = std::from_chars(
            raw_value.data(), raw_value.data() + raw_value.size(), value);
        if (error != std::errc{} || end != raw_value.data() + raw_value.size() ||
            !(fields >> unit) || unit != "kB") {
            throw HealthMonitorError("procfs memory statistics are malformed");
        }
        values[key] = value;
    }
    if (!values.contains("MemTotal") || !values.contains("MemAvailable") ||
        !values.contains("SwapTotal") || !values.contains("SwapFree")) {
        throw HealthMonitorError("procfs memory statistics are incomplete");
    }
    const auto total = values.at("MemTotal");
    const auto available = values.at("MemAvailable");
    const auto swap_total = values.at("SwapTotal");
    const auto swap_free = values.at("SwapFree");
    if (total == 0 || available > total || swap_free > swap_total) {
        throw HealthMonitorError("procfs memory counters are inconsistent");
    }
    const auto memory = 100.0 * static_cast<double>(total - available) /
                        static_cast<double>(total);
    const auto swap = swap_total == 0
                          ? 0.0
                          : 100.0 * static_cast<double>(swap_total - swap_free) /
                                static_cast<double>(swap_total);
    return {memory, swap};
}

double DeterministicHealthMonitor::read_disk_percent() const {
    struct statvfs usage {};
    if (statvfs(disk_path_.c_str(), &usage) != 0 || usage.f_blocks == 0) {
        throw HealthMonitorError("cannot read filesystem usage");
    }
    const auto used = usage.f_blocks - std::min(usage.f_bfree, usage.f_blocks);
    return 100.0 * static_cast<double>(used) /
           static_cast<double>(usage.f_blocks);
}

double DeterministicHealthMonitor::read_load_percent() const {
    const auto words = split_words(read_file(proc_root_ + "/loadavg"));
    if (words.empty()) {
        throw HealthMonitorError("procfs load average is malformed");
    }
    std::size_t parsed = 0;
    double load = 0;
    try {
        load = std::stod(words[0], &parsed);
    } catch (const std::exception&) {
        throw HealthMonitorError("procfs load average is malformed");
    }
    const auto cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (parsed != words[0].size() || !std::isfinite(load) || load < 0 ||
        cpu_count <= 0) {
        throw HealthMonitorError("procfs load average or CPU count is invalid");
    }
    return 100.0 * load / static_cast<double>(cpu_count);
}

HealthMetric DeterministicHealthMonitor::percent_metric(
    const std::string& name, std::optional<double> value) const {
    if (!value.has_value()) {
        return {name, std::nullopt, "percent", HealthLevel::unknown};
    }
    if (!std::isfinite(*value) || *value < 0 ||
        (name != "load" && *value > 100)) {
        throw HealthMonitorError("health percentage is invalid");
    }
    const auto level = *value >= thresholds_.critical_percent
                           ? HealthLevel::critical
                           : *value >= thresholds_.warning_percent
                                 ? HealthLevel::warning
                                 : HealthLevel::healthy;
    return {name, value, "percent", level};
}

std::string to_string(HealthLevel level) {
    switch (level) {
    case HealthLevel::healthy:
        return "healthy";
    case HealthLevel::warning:
        return "warning";
    case HealthLevel::critical:
        return "critical";
    case HealthLevel::unknown:
        return "unknown";
    }
    throw std::invalid_argument("invalid health level");
}

void register_health_tools(
    ToolBroker& broker,
    std::shared_ptr<DeterministicHealthMonitor> monitor) {
    if (!monitor) {
        throw std::invalid_argument("health monitor must not be null");
    }
    broker.register_tool(
        {"system.get_health",
         {{"type", "object"},
          {"properties", nlohmann::json::object()},
          {"additionalProperties", false}},
         ToolRisk::read_only},
        [monitor = std::move(monitor)](const nlohmann::json&) {
            try {
                return health_snapshot_to_json(monitor->sample());
            } catch (const HealthMonitorError&) {
                throw ToolExecutionError("health_sampling_failed");
            }
        });
}

} // namespace ailinux::ai
