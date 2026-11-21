#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ailinux::core {

class ReleaseRecoveryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ReleaseAuditEntry {
    std::string timestamp;
    std::string action;
    std::string version;
};

class ReleaseRecoveryManager {
public:
    explicit ReleaseRecoveryManager(std::string directory);

    void record_stable(const std::string& version,
                       const std::string& sha256);
    void prepare_candidate(const std::string& version,
                           const std::string& sha256);
    void mark_candidate_verified(const std::string& version,
                                 bool passed);
    void activate_candidate(const std::string& version);
    void rollback();

    [[nodiscard]] nlohmann::json status() const;
    [[nodiscard]] std::vector<ReleaseAuditEntry> audit() const;

private:
    std::string directory_;
    std::string state_path_;
    std::string audit_path_;

    [[nodiscard]] nlohmann::json load_state() const;
    void save_state(const nlohmann::json& state) const;
    void append_audit(const std::string& action,
                      const std::string& version) const;
};

} // namespace ailinux::core
