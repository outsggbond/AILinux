#include "super_self_workspace.hpp"

#include <cerrno>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>

namespace ailinux::core {
namespace {

std::string real_path(const std::string& path) {
    std::error_code error;
    auto result = std::filesystem::canonical(path, error);
    if (error) {
        throw SuperSelfWorkspaceError("path does not exist: " + path);
    }
    return result.string();
}

bool is_within(const std::filesystem::path& path,
               const std::filesystem::path& parent) {
    auto p = path.lexically_normal();
    auto q = parent.lexically_normal();
    auto pi = p.begin();
    auto qi = q.begin();
    for (; pi != p.end() && qi != q.end(); ++pi, ++qi) {
        if (*pi != *qi) return false;
    }
    return qi == q.end();
}

} // namespace

SuperSelfWorkspace::SuperSelfWorkspace(std::string source_root,
                                       std::string workspace_root)
    : source_root_(real_path(source_root)) {
    namespace fs = std::filesystem;
    fs::path workspace = fs::absolute(workspace_root).lexically_normal();
    workspace_root_ = workspace.string();

    if (is_within(workspace_root_, source_root_)) {
        throw SuperSelfWorkspaceError("workspace must not be inside the source repository");
    }
}

void SuperSelfWorkspace::create() {
    namespace fs = std::filesystem;
    fs::path parent = fs::path(workspace_root_).parent_path();
    fs::create_directories(parent);
    chmod(parent.c_str(), 0700);

    struct stat parent_info {};
    if (stat(parent.c_str(), &parent_info) != 0 ||
        parent_info.st_uid != geteuid() ||
        (parent_info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        throw SuperSelfWorkspaceError("workspace parent must be owned and private");
    }

    if (fs::exists(workspace_root_) || fs::is_symlink(workspace_root_) ||
        fs::exists(workspace_root_ + "/.git") ) {
        throw SuperSelfWorkspaceError("workspace already exists");
    }

    const auto result = run({"git", "-C", source_root_, "worktree", "add",
                             "--detach", workspace_root_, "HEAD"},
                            source_root_);
    if (result.exit_code != 0) {
        throw SuperSelfWorkspaceError("git worktree creation failed: " + result.output);
    }
    chmod(workspace_root_.c_str(), 0700);
}

void SuperSelfWorkspace::remove() {
    const auto result = run({"git", "-C", source_root_, "worktree", "remove",
                             "--force", workspace_root_}, source_root_);
    if (result.exit_code != 0) {
        throw SuperSelfWorkspaceError("git worktree removal failed: " + result.output);
    }
}

WorkspaceCommandResult SuperSelfWorkspace::build(
    const std::string& build_directory) const {
    namespace fs = std::filesystem;
    if (!fs::exists(workspace_root_) || fs::is_symlink(workspace_root_)) {
        throw SuperSelfWorkspaceError("workspace does not exist");
    }
    const fs::path build = fs::path(build_directory);
    if (build.is_absolute() || build.empty()) {
        throw SuperSelfWorkspaceError("build directory must be workspace-relative");
    }
    for (const auto& component : build) {
        if (component == "..") {
            throw SuperSelfWorkspaceError("build directory must not escape the workspace");
        }
    }

    const std::string sysroot = source_root_ + "/build/native-deps/sysroot";
    std::vector<std::string> configure_arguments = {
        "cmake", "-S", workspace_root_, "-B",
        workspace_root_ + "/" + build_directory, "-G", "Ninja",
        "-DSQLite3_INCLUDE_DIR=" + sysroot + "/usr/include",
        "-DSQLite3_LIBRARY=/usr/lib/x86_64-linux-gnu/libsqlite3.so.0",
        "-DCURL_INCLUDE_DIR=" + sysroot + "/usr/include/x86_64-linux-gnu",
        "-DCURL_LIBRARY_RELEASE=/usr/lib/x86_64-linux-gnu/libcurl.so.4",
        "-DCURL_LIBRARY_DEBUG=/usr/lib/x86_64-linux-gnu/libcurl.so.4",
        "-Dnlohmann_json_DIR=" + sysroot + "/usr/share/cmake/nlohmann_json"
    };
    const auto result = run(configure_arguments, workspace_root_);
    if (result.exit_code != 0) return result;
    return run({"cmake", "--build", workspace_root_ + "/" + build_directory},
               workspace_root_);
}

WorkspaceCommandResult SuperSelfWorkspace::test(
    const std::string& build_directory) const {
    const std::string build_path = workspace_root_ + "/" + build_directory;
    return run({"ctest", "--test-dir", build_path, "--output-on-failure"},
               workspace_root_);
}

const std::string& SuperSelfWorkspace::source_root() const noexcept {
    return source_root_;
}

const std::string& SuperSelfWorkspace::workspace_root() const noexcept {
    return workspace_root_;
}

WorkspaceCommandResult SuperSelfWorkspace::run(
    const std::vector<std::string>& arguments,
    const std::string& working_directory) const {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        throw SuperSelfWorkspaceError("cannot create workspace command pipe");
    }

    fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);

    const pid_t child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        throw SuperSelfWorkspaceError("cannot fork workspace command");
    }
    if (child == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        if (chdir(working_directory.c_str()) != 0) _exit(126);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipe_fds[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t count = read(pipe_fds[0], buffer.data(), buffer.size());
        if (count > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(count));
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            close(pipe_fds[0]);
            throw SuperSelfWorkspaceError("cannot read workspace command output");
        }
    }
    close(pipe_fds[0]);

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        throw SuperSelfWorkspaceError("cannot wait for workspace command");
    }
    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return {exit_code, std::move(output)};
}

} // namespace ailinux::core
