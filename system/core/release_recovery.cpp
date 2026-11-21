#include "release_recovery.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <ctime>

namespace ailinux::core {
namespace {

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm value{};
    gmtime_r(&seconds, &value);
    std::array<char, 32> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &value) == 0) {
        throw ReleaseRecoveryError("cannot format recovery timestamp");
    }
    return buffer.data();
}

bool valid_sha256(const std::string& value) {
    if (value.size() != 64) return false;
    for (char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool valid_version(const std::string& value) {
    return !value.empty() && value.size() <= 128;
}

} // namespace

ReleaseRecoveryManager::ReleaseRecoveryManager(std::string directory)
    : directory_(std::move(directory)),
      state_path_(directory_ + "/release-state.json"),
      audit_path_(directory_ + "/recovery-audit.jsonl") {
    namespace fs = std::filesystem;
    fs::create_directories(directory_);
    if (chmod(directory_.c_str(), 0700) != 0) {
        throw ReleaseRecoveryError("cannot restrict release recovery directory");
    }

    struct stat info {};
    if (stat(directory_.c_str(), &info) != 0 ||
        info.st_uid != geteuid() ||
        (info.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        !S_ISDIR(info.st_mode)) {
        throw ReleaseRecoveryError("release recovery directory must be owned and private");
    }

    if (!fs::exists(state_path_)) {
        nlohmann::json initial = {
            {"schema_version", 1},
            {"stable", nullptr},
            {"current", nullptr},
            {"rollback", nullptr},
            {"candidate", nullptr}
        };
        save_state(initial);
        append_audit("initialize", "none");
    } else {
        (void)load_state();
    }
}

nlohmann::json ReleaseRecoveryManager::load_state() const {
    std::ifstream input(state_path_);
    if (!input) throw ReleaseRecoveryError("cannot open release state");
    nlohmann::json state;
    try {
        input >> state;
    } catch (const nlohmann::json::exception&) {
        throw ReleaseRecoveryError("release state is malformed");
    }
    if (!state.is_object() || state.value("schema_version", 0) != 1) {
        throw ReleaseRecoveryError("unsupported release state schema");
    }
    return state;
}

void ReleaseRecoveryManager::save_state(const nlohmann::json& state) const {
    const std::string temporary = state_path_ + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) throw ReleaseRecoveryError("cannot open release state temporary file");
        output << state.dump(2) << '\n';
        output.flush();
        if (!output) throw ReleaseRecoveryError("cannot write release state");
    }
    if (chmod(temporary.c_str(), 0600) != 0) {
        throw ReleaseRecoveryError("cannot restrict release state permissions");
    }
    if (rename(temporary.c_str(), state_path_.c_str()) != 0) {
        unlink(temporary.c_str());
        throw ReleaseRecoveryError("cannot atomically replace release state");
    }
}

void ReleaseRecoveryManager::append_audit(const std::string& action,
                                          const std::string& version) const {
    std::ofstream output(audit_path_, std::ios::app);
    if (!output) throw ReleaseRecoveryError("cannot open recovery audit log");
    nlohmann::json entry = {{"timestamp", utc_timestamp()},
                            {"action", action},
                            {"version", version}};
    output << entry.dump() << '\n';
    output.flush();
    if (!output) throw ReleaseRecoveryError("cannot append recovery audit log");
    chmod(audit_path_.c_str(), 0600);
}

void ReleaseRecoveryManager::record_stable(const std::string& version,
                                           const std::string& sha256) {
    if (!valid_version(version) || !valid_sha256(sha256)) {
        throw ReleaseRecoveryError("invalid stable release version or SHA-256");
    }
    auto state = load_state();
    state["stable"] = {{"version", version}, {"sha256", sha256},
                        {"recorded_at", utc_timestamp()}};
    if (state["current"].is_null()) state["current"] = state["stable"];
    if (state["rollback"].is_null()) state["rollback"] = state["stable"];
    save_state(state);
    append_audit("record_stable", version);
}

void ReleaseRecoveryManager::prepare_candidate(const std::string& version,
                                               const std::string& sha256) {
    if (!valid_version(version) || !valid_sha256(sha256)) {
        throw ReleaseRecoveryError("invalid candidate version or SHA-256");
    }
    auto state = load_state();
    state["candidate"] = {{"version", version}, {"sha256", sha256},
                           {"verified", false},
                           {"prepared_at", utc_timestamp()}};
    save_state(state);
    append_audit("prepare_candidate", version);
}

void ReleaseRecoveryManager::mark_candidate_verified(const std::string& version,
                                                     bool passed) {
    if (!valid_version(version)) throw ReleaseRecoveryError("invalid candidate version");
    auto state = load_state();
    if (state["candidate"].is_null() ||
        state["candidate"].value("version", "") != version) {
        throw ReleaseRecoveryError("candidate version does not match prepared state");
    }
    if (!passed) {
        throw ReleaseRecoveryError("candidate verification failed; activation denied");
    }
    state["candidate"]["verified"] = true;
    state["candidate"]["verified_at"] = utc_timestamp();
    save_state(state);
    append_audit("verify_candidate", version);
}

void ReleaseRecoveryManager::activate_candidate(const std::string& version) {
    if (!valid_version(version)) throw ReleaseRecoveryError("invalid candidate version");
    auto state = load_state();
    if (state["candidate"].is_null() ||
        state["candidate"].value("version", "") != version ||
        state["candidate"].value("verified", false) != true) {
        throw ReleaseRecoveryError("candidate is missing or has not passed verification");
    }
    state["rollback"] = state["current"];
    state["current"] = state["candidate"];
    state["candidate"] = nullptr;
    save_state(state);
    append_audit("activate_candidate", version);
}

void ReleaseRecoveryManager::rollback() {
    auto state = load_state();
    if (state["rollback"].is_null()) {
        throw ReleaseRecoveryError("no rollback release is recorded");
    }
    if (!state["current"].is_null()) {
        nlohmann::json rejected = state["current"];
        state["candidate"] = rejected;
    }
    state["current"] = state["rollback"];
    state["rollback"] = state["stable"];
    save_state(state);
    append_audit("rollback", state["current"].value("version", "unknown"));
}

nlohmann::json ReleaseRecoveryManager::status() const {
    return load_state();
}

std::vector<ReleaseAuditEntry> ReleaseRecoveryManager::audit() const {
    std::vector<ReleaseAuditEntry> entries;
    std::ifstream input(audit_path_);
    if (!input) return entries;
    std::string line;
    while (std::getline(input, line)) {
        try {
            auto entry = nlohmann::json::parse(line);
            entries.push_back({entry.value("timestamp", ""),
                               entry.value("action", ""),
                               entry.value("version", "")});
        } catch (const nlohmann::json::exception&) {
            throw ReleaseRecoveryError("recovery audit log contains malformed records");
        }
    }
    return entries;
}

} // namespace ailinux::core
