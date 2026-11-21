#include "release_recovery.hpp"

#include <filesystem>
#include <iostream>
#include <cstdlib>
#include <sys/stat.h>
#include <string>

namespace fs = std::filesystem;

int main() {
    try {
        char parent_template[] = "/tmp/ailinux-release-test-XXXXXX";
        char* parent = mkdtemp(parent_template);
        if (parent == nullptr) throw ailinux::core::ReleaseRecoveryError("cannot create test directory");
        chmod(parent, 0700);

        ailinux::core::ReleaseRecoveryManager manager(parent);
        const std::string stable_hash(64, 'a');
        const std::string candidate_hash(64, 'b');
        manager.record_stable("v1", stable_hash);
        manager.prepare_candidate("v2", candidate_hash);

        bool denied = false;
        try { manager.activate_candidate("v2"); }
        catch (const ailinux::core::ReleaseRecoveryError&) { denied = true; }
        if (!denied) throw ailinux::core::ReleaseRecoveryError("unverified candidate was activated");

        manager.mark_candidate_verified("v2", true);
        manager.activate_candidate("v2");
        auto active = manager.status();
        if (active["current"]["version"] != "v2" ||
            active["rollback"]["version"] != "v1") {
            throw ailinux::core::ReleaseRecoveryError("candidate activation state is incorrect");
        }

        manager.rollback();
        active = manager.status();
        if (active["current"]["version"] != "v1") {
            throw ailinux::core::ReleaseRecoveryError("rollback state is incorrect");
        }
        const auto audit = manager.audit();
        if (audit.size() < 5) throw ailinux::core::ReleaseRecoveryError("recovery audit is incomplete");

        fs::remove_all(parent);
        std::cout << "Release recovery test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
