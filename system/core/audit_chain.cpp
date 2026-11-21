#include "audit_chain.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace ailinux::core {
namespace {

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm value{};
    gmtime_r(&seconds, &value);
    std::array<char, 32> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &value) == 0) {
        throw AuditChainError("cannot format audit timestamp");
    }
    return buffer.data();
}

std::string hex(const unsigned char* data, std::size_t size) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        output << std::setw(2) << static_cast<unsigned int>(data[index]);
    }
    return output.str();
}

} // namespace

AuditChain::AuditChain(std::string directory)
    : directory_(std::move(directory)),
      log_path_(directory_ + "/audit.jsonl"),
      key_path_(directory_ + "/audit.key"),
      head_path_(directory_ + "/audit.head") {
    namespace fs = std::filesystem;
    fs::create_directories(directory_);
    if (chmod(directory_.c_str(), 0700) != 0) {
        throw AuditChainError("cannot restrict audit directory");
    }

    struct stat info {};
    if (stat(directory_.c_str(), &info) != 0 ||
        info.st_uid != geteuid() ||
        (info.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        !S_ISDIR(info.st_mode)) {
        throw AuditChainError("audit directory must be owned and private");
    }

    if (!fs::exists(key_path_)) {
        unsigned char key[32]{};
        if (RAND_bytes(key, sizeof(key)) != 1) {
            throw AuditChainError("cannot generate audit HMAC key");
        }
        std::ofstream output(key_path_, std::ios::binary | std::ios::trunc);
        if (!output) throw AuditChainError("cannot create audit HMAC key");
        output.write(reinterpret_cast<const char*>(key), sizeof(key));
        output.close();
        if (chmod(key_path_.c_str(), 0600) != 0) {
            throw AuditChainError("cannot restrict audit HMAC key");
        }
    } else {
        if (stat(key_path_.c_str(), &info) != 0 ||
            info.st_uid != geteuid() || (info.st_mode & 0077) != 0 ||
            info.st_size != 32 || !S_ISREG(info.st_mode)) {
            throw AuditChainError("audit HMAC key permissions or size are unsafe");
        }
    }

    if (!fs::exists(log_path_)) {
        std::ofstream output(log_path_, std::ios::binary | std::ios::trunc);
        if (!output) throw AuditChainError("cannot create audit log");
        output.close();
        if (chmod(log_path_.c_str(), 0600) != 0) {
            throw AuditChainError("cannot restrict audit log permissions");
        }
    } else {
        if (stat(log_path_.c_str(), &info) != 0 ||
            info.st_uid != geteuid() || (info.st_mode & 0077) != 0 ||
            !S_ISREG(info.st_mode)) {
            throw AuditChainError("audit log permissions are unsafe");
        }
    }

    if (!fs::exists(head_path_)) {
        nlohmann::json head = {{"seq", 0},
                               {"record_hash", std::string(64, '0')}};
        std::ofstream output(head_path_, std::ios::binary | std::ios::trunc);
        if (!output) throw AuditChainError("cannot create audit chain head");
        output << head.dump() << '\n';
        output.close();
        if (chmod(head_path_.c_str(), 0600) != 0) {
            throw AuditChainError("cannot restrict audit chain head");
        }
    } else if (stat(head_path_.c_str(), &info) != 0 ||
               info.st_uid != geteuid() || (info.st_mode & 0077) != 0 ||
               !S_ISREG(info.st_mode)) {
        throw AuditChainError("audit chain head permissions are unsafe");
    }
}

std::string AuditChain::sha256(const std::string& payload) const {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) throw AuditChainError("cannot allocate SHA-256 context");
    unsigned int size = 0;
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context, payload.data(), payload.size()) != 1 ||
        EVP_DigestFinal_ex(context, digest, &size) != 1) {
        EVP_MD_CTX_free(context);
        throw AuditChainError("cannot hash audit payload");
    }
    EVP_MD_CTX_free(context);
    return hex(digest, size);
}

std::string AuditChain::hmac_sha256(const std::string& message) const {
    std::ifstream input(key_path_, std::ios::binary);
    if (!input) throw AuditChainError("cannot open audit HMAC key");
    std::string key((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
    if (key.size() != 32) throw AuditChainError("audit HMAC key is invalid");

    unsigned int size = 0;
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(message.data()), message.size(),
             digest, &size) == nullptr) {
        throw AuditChainError("cannot calculate audit record HMAC");
    }
    return hex(digest, size);
}

AuditRecord AuditChain::append(const std::string& trace_id,
                               const std::string& system_state,
                               const std::string& agent_id,
                               const std::string& action_type,
                               const std::string& decision,
                               const nlohmann::json& payload) {
    if (trace_id.empty() || trace_id.size() > 128 ||
        system_state.empty() || agent_id.empty() ||
        action_type.empty() || decision.empty()) {
        throw AuditChainError("audit record fields are invalid");
    }

    auto existing = verify();
    const std::uint64_t seq = existing.empty() ? 1 : existing.back().seq + 1;
    const std::string prev = existing.empty() ? std::string(64, '0')
                                               : existing.back().record_hash;
    AuditRecord record;
    record.seq = seq;
    record.timestamp = utc_timestamp();
    record.trace_id = trace_id;
    record.system_state = system_state;
    record.agent_id = agent_id;
    record.action_type = action_type;
    record.decision = decision;
    record.payload_sha256 = sha256(payload.dump());
    record.prev_hash = prev;

    std::ostringstream message;
    message << seq << '|' << record.timestamp << '|' << trace_id << '|'
            << record.payload_sha256 << '|' << prev;
    record.record_hash = hmac_sha256(message.str());

    nlohmann::json json = {{"seq", record.seq},
                           {"timestamp", record.timestamp},
                           {"trace_id", record.trace_id},
                           {"system_state", record.system_state},
                           {"agent_id", record.agent_id},
                           {"action_type", record.action_type},
                           {"decision", record.decision},
                           {"payload_sha256", record.payload_sha256},
                           {"prev_hash", record.prev_hash},
                           {"record_hash", record.record_hash}};

    int descriptor = open(log_path_.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) throw AuditChainError("cannot open audit log for append");
    const std::string line = json.dump() + '\n';
    std::size_t written = 0;
    while (written < line.size()) {
        const ssize_t count = write(descriptor, line.data() + written,
                                    line.size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            close(descriptor);
            throw AuditChainError("cannot append audit record");
        }
    }
    if (fsync(descriptor) != 0) {
        close(descriptor);
        throw AuditChainError("ERR_AUDIT_FSYNC_FAILED");
    }
    close(descriptor);

    const std::string head_temp = head_path_ + ".tmp";
    nlohmann::json head = {{"seq", record.seq},
                           {"record_hash", record.record_hash}};
    {
        std::ofstream output(head_temp, std::ios::binary | std::ios::trunc);
        if (!output) throw AuditChainError("cannot open audit chain head temporary file");
        output << head.dump() << '\n';
        output.flush();
        if (!output) throw AuditChainError("cannot write audit chain head");
    }
    if (chmod(head_temp.c_str(), 0600) != 0) {
        throw AuditChainError("cannot restrict audit chain head temporary file");
    }
    int head_fd = open(head_temp.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (head_fd < 0 || fsync(head_fd) != 0) {
        if (head_fd >= 0) close(head_fd);
        throw AuditChainError("ERR_AUDIT_FSYNC_FAILED");
    }
    close(head_fd);
    if (rename(head_temp.c_str(), head_path_.c_str()) != 0) {
        unlink(head_temp.c_str());
        throw AuditChainError("cannot atomically replace audit chain head");
    }
    return record;
}

std::vector<AuditRecord> AuditChain::verify() const {
    std::ifstream input(log_path_, std::ios::binary);
    if (!input) throw AuditChainError("cannot open audit log");

    std::vector<AuditRecord> records;
    std::string prev = std::string(64, '0');
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        nlohmann::json json;
        try {
            json = nlohmann::json::parse(line);
        } catch (const nlohmann::json::exception&) {
            throw AuditChainError("audit log contains malformed JSON");
        }
        AuditRecord record;
        try {
            record.seq = json.at("seq").get<std::uint64_t>();
            record.timestamp = json.at("timestamp").get<std::string>();
            record.trace_id = json.at("trace_id").get<std::string>();
            record.system_state = json.at("system_state").get<std::string>();
            record.agent_id = json.at("agent_id").get<std::string>();
            record.action_type = json.at("action_type").get<std::string>();
            record.decision = json.at("decision").get<std::string>();
            record.payload_sha256 = json.at("payload_sha256").get<std::string>();
            record.prev_hash = json.at("prev_hash").get<std::string>();
            record.record_hash = json.at("record_hash").get<std::string>();
        } catch (const nlohmann::json::exception&) {
            throw AuditChainError("audit log contains missing fields");
        }

        const std::uint64_t expected_seq = records.empty() ? 1 : records.back().seq + 1;
        if (record.seq != expected_seq || record.prev_hash != prev) {
            throw AuditChainError("audit chain sequence or deletion detected");
        }
        std::ostringstream message;
        message << record.seq << '|' << record.timestamp << '|'
                << record.trace_id << '|' << record.payload_sha256 << '|'
                << record.prev_hash;
        if (hmac_sha256(message.str()) != record.record_hash) {
            throw AuditChainError("audit record HMAC mismatch or tampering detected");
        }
        prev = record.record_hash;
        records.push_back(std::move(record));
    }
    std::ifstream head_input(head_path_, std::ios::binary);
    if (!head_input) throw AuditChainError("cannot open audit chain head");
    nlohmann::json head;
    try {
        head_input >> head;
        const std::uint64_t head_seq = head.at("seq").get<std::uint64_t>();
        const std::string head_hash = head.at("record_hash").get<std::string>();
        if (records.empty()) {
            if (head_seq != 0 || head_hash != std::string(64, '0')) {
                throw AuditChainError("audit deletion detected against chain head");
            }
        } else if (head_seq != records.back().seq ||
                   head_hash != records.back().record_hash) {
            throw AuditChainError("audit deletion or chain-head tampering detected");
        }
    } catch (const nlohmann::json::exception&) {
        throw AuditChainError("audit chain head is malformed");
    }
    return records;
}

} // namespace ailinux::core
