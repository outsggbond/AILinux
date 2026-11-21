#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace ailinux::core {

class SuperSelfWorkspaceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct WorkspaceCommandResult {
    int exit_code = 0;
    std::string output;
};

class SuperSelfWorkspace {
public:
    explicit SuperSelfWorkspace(std::string source_root,
                                std::string workspace_root);

    void create();
    void remove();
    [[nodiscard]] WorkspaceCommandResult build(
        const std::string& build_directory = "build/super-self") const;
    [[nodiscard]] WorkspaceCommandResult test(
        const std::string& build_directory = "build/super-self") const;

    [[nodiscard]] const std::string& source_root() const noexcept;
    [[nodiscard]] const std::string& workspace_root() const noexcept;

private:
    std::string source_root_;
    std::string workspace_root_;

    [[nodiscard]] WorkspaceCommandResult run(
        const std::vector<std::string>& arguments,
        const std::string& working_directory) const;
};

} // namespace ailinux::core
