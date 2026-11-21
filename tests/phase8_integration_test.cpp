#include "health_monitor.hpp"
#include "release_recovery.hpp"
#include "super_self_workspace.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

int main() {
    try {
        char release_template[] = "/tmp/ailinux-phase8-release-XXXXXX";
        char* release_parent = mkdtemp(release_template);
        if (release_parent == nullptr) throw std::runtime_error("cannot create release directory");
        chmod(release_parent, 0700);

        ailinux::core::ReleaseRecoveryManager releases(release_parent);
        const std::string stable_hash(64, '1');
        const std::string candidate_hash(64, '2');
        releases.record_stable("v1.0", stable_hash);
        releases.prepare_candidate("v1.1", candidate_hash);

        try {
            releases.activate_candidate("v1.1");
            throw std::runtime_error("unverified release was activated");
        } catch (const ailinux::core::ReleaseRecoveryError&) {
        }

        releases.mark_candidate_verified("v1.1", true);
        releases.activate_candidate("v1.1");
        releases.rollback();
        if (releases.status()["current"]["version"] != "v1.0") {
            throw std::runtime_error("release rollback did not restore stable state");
        }

        const fs::path release_state = fs::path(release_parent) / "release-state.json";
        std::ofstream broken(release_state, std::ios::trunc);
        broken << '{';
        broken.close();
        try {
            (void)releases.status();
            throw std::runtime_error("malformed release state was accepted");
        } catch (const ailinux::core::ReleaseRecoveryError&) {
        }

        char workspace_template[] = "/tmp/ailinux-phase8-workspace-XXXXXX";
        char* workspace_parent = mkdtemp(workspace_template);
        if (workspace_parent == nullptr) throw std::runtime_error("cannot create workspace directory");
        chmod(workspace_parent, 0700);

#ifndef AILINUX_SOURCE_DIR
#error AILINUX_SOURCE_DIR must be defined
#endif
        const fs::path source = fs::canonical(AILINUX_SOURCE_DIR);
        const fs::path workspace = fs::path(workspace_parent) / "super-self";
        ailinux::core::SuperSelfWorkspace super(source.string(), workspace.string());
        super.create();
        if (!fs::exists(workspace / ".git") && !fs::is_symlink(workspace / ".git")) {
            throw std::runtime_error("Super Self worktree was not created");
        }
        super.remove();

        ailinux::ai::DeterministicHealthMonitor health("/proc", workspace_parent);
        const auto snapshot = health.sample();
        const auto health_json = ailinux::ai::health_snapshot_to_json(snapshot);
        if (!health_json.contains("schema_version") ||
            !health_json.contains("metrics") ||
            health_json["schema_version"] != 1) {
            throw std::runtime_error("health JSON contract is invalid");
        }

        fs::remove_all(workspace_parent);
        fs::remove_all(release_parent);
        std::cout << "Phase 8 integration regression passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
