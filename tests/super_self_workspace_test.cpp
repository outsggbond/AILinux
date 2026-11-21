#include "super_self_workspace.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <sys/stat.h>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

int main() {
    try {
#ifndef AILINUX_SOURCE_DIR
#error AILINUX_SOURCE_DIR must be defined
#endif
        const fs::path source = fs::canonical(AILINUX_SOURCE_DIR);
        char template_name[] = "/tmp/ailinux-super-self-test-XXXXXX";
        char* parent = mkdtemp(template_name);
        if (parent == nullptr) throw ailinux::core::SuperSelfWorkspaceError("cannot create test parent");
        chmod(parent, 0700);
        const fs::path workspace = fs::path(parent) / "workspace";

        ailinux::core::SuperSelfWorkspace environment(source.string(), workspace.string());
        environment.create();
        if (!fs::exists(workspace / ".git") && !fs::is_symlink(workspace / ".git")) {
            throw ailinux::core::SuperSelfWorkspaceError("worktree did not create metadata");
        }
        if ((fs::status(workspace).permissions() & fs::perms::group_all) != fs::perms::none) {
            throw ailinux::core::SuperSelfWorkspaceError("workspace mode is not private");
        }

        auto build = environment.build("build/super-self-test");
        if (build.exit_code != 0) {
            std::cerr << build.output;
            throw ailinux::core::SuperSelfWorkspaceError("workspace build failed");
        }
        environment.remove();
        fs::remove_all(parent);
        std::cout << "Super Self workspace test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
