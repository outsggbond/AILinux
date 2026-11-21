#include "audit_chain.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <cstdlib>

int main() {
    try {
        char template_name[] = "/tmp/ailinux-audit-chain-XXXXXX";
        char* parent = mkdtemp(template_name);
        if (parent == nullptr) throw ailinux::core::AuditChainError("cannot create audit test directory");
        chmod(parent, 0700);

        ailinux::core::AuditChain audit(parent);
        audit.append("trace-1", "S1_EGO_NORMAL", "agent-a", "STATE_CHANGE", "EXEC_OK",
                     nlohmann::json{{"to", "S2_EGO_DEGRADED"}});
        audit.append("trace-2", "S2_EGO_DEGRADED", "agent-a", "ROLLBACK", "ALLOW",
                     nlohmann::json{{"version", "v1"}});
        auto records = audit.verify();
        if (records.size() != 2 || records[0].seq != 1 || records[1].prev_hash != records[0].record_hash) {
            throw ailinux::core::AuditChainError("audit chain verification mismatch");
        }

        const std::string log_path = std::string(parent) + "/audit.jsonl";
        std::ifstream input(log_path);
        std::string first_line;
        std::string second_line;
        std::getline(input, first_line);
        std::getline(input, second_line);
        input.close();

        std::ofstream deletion(log_path, std::ios::trunc);
        deletion << first_line << '\n';
        deletion.close();
        try {
            (void)audit.verify();
            throw ailinux::core::AuditChainError("deleted audit record was accepted");
        } catch (const ailinux::core::AuditChainError&) {
        }

        std::ofstream restore(log_path, std::ios::trunc);
        restore << first_line << '\n' << second_line << '\n';
        restore.close();
        std::fstream log(log_path, std::ios::in | std::ios::out | std::ios::binary);
        log.seekp(static_cast<std::streamoff>(20), std::ios::beg);
        log.put('X');
        log.close();
        try {
            (void)audit.verify();
            throw ailinux::core::AuditChainError("tampered audit record was accepted");
        } catch (const ailinux::core::AuditChainError&) {
        }

        std::filesystem::remove_all(parent);
        std::cout << "Audit hash-chain test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
