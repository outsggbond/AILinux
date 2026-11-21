#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace ailinux::core {

class AuditChainError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct AuditRecord {
    std::uint64_t seq = 0;
    std::string timestamp;
    std::string trace_id;
    std::string system_state;
    std::string agent_id;
    std::string action_type;
    std::string decision;
    std::string payload_sha256;
    std::string prev_hash;
    std::string record_hash;
};

class AuditChain {
public:
    explicit AuditChain(std::string directory);

    AuditRecord append(const std::string& trace_id,
                       const std::string& system_state,
                       const std::string& agent_id,
                       const std::string& action_type,
                       const std::string& decision,
                       const nlohmann::json& payload);

    [[nodiscard]] std::vector<AuditRecord> verify() const;

private:
    std::string directory_;
    std::string log_path_;
    std::string key_path_;
    std::string head_path_;

    [[nodiscard]] std::string hmac_sha256(
        const std::string& message) const;
    [[nodiscard]] std::string sha256(const std::string& payload) const;
};

} // namespace ailinux::core
