#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace ailinux::ai {

enum class HealthLevel { healthy, warning, critical, unknown };

struct HealthThresholds {
    double warning_percent = 85.0;
    double critical_percent = 95.0;
};

struct HealthMetric {
    std::string name;
    std::optional<double> value;
    std::string unit;
    HealthLevel level;
};

struct HealthSnapshot {
    std::string sampled_at;
    std::vector<HealthMetric> metrics;

    [[nodiscard]] HealthLevel overall() const;
};

[[nodiscard]] nlohmann::json
health_snapshot_to_json(const HealthSnapshot& snapshot);

class HealthMonitorError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class DeterministicHealthMonitor {
public:
    explicit DeterministicHealthMonitor(std::string proc_root = "/proc",
                                        std::string disk_path = "/",
                                        HealthThresholds thresholds = {});

    [[nodiscard]] HealthSnapshot sample(
        const std::map<std::string, int>& expected_processes = {});

private:
    [[nodiscard]] std::optional<double> read_cpu_percent();
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t>
    read_network_counters() const;
    [[nodiscard]] std::pair<std::optional<double>, std::optional<double>>
    read_network_rates();
    [[nodiscard]] HealthMetric read_oom_kill_delta();
    [[nodiscard]] std::pair<double, double> read_memory_percentages() const;
    [[nodiscard]] double read_disk_percent() const;
    [[nodiscard]] double read_load_percent() const;
    [[nodiscard]] HealthMetric percent_metric(const std::string& name,
                                             std::optional<double> value) const;

    std::string proc_root_;
    std::string disk_path_;
    HealthThresholds thresholds_;
    std::mutex sample_mutex_;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> previous_cpu_;
    std::chrono::steady_clock::time_point previous_sample_time_{};
    std::optional<std::pair<std::uint64_t, std::uint64_t>> previous_network_;
    std::chrono::steady_clock::time_point previous_network_sample_time_{};
    std::optional<std::uint64_t> previous_oom_kills_;
};

[[nodiscard]] std::string to_string(HealthLevel level);

class ToolBroker;

void register_health_tools(
    ToolBroker& broker,
    std::shared_ptr<DeterministicHealthMonitor> monitor);

} // namespace ailinux::ai
