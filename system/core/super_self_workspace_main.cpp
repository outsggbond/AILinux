#include "super_self_workspace.hpp"

#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    try {
        if (argc < 4) {
            std::cout << "Usage: ailinux-super-self --source PATH --workspace PATH create|remove|build|test\n";
            return 2;
        }
        std::string source;
        std::string workspace;
        std::string action;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "--source" && index + 1 < argc) source = argv[++index];
            else if (argument == "--workspace" && index + 1 < argc) workspace = argv[++index];
            else if (!argument.starts_with("-") && action.empty()) action = argument;
            else throw std::invalid_argument("unsupported or incomplete option");
        }
        if (source.empty() || workspace.empty() || action.empty()) {
            throw std::invalid_argument("source, workspace, and action are required");
        }
        ailinux::core::SuperSelfWorkspace environment(source, workspace);
        if (action == "create") environment.create();
        else if (action == "remove") environment.remove();
        else if (action == "build") {
            auto result = environment.build();
            std::cout << result.output;
            return result.exit_code;
        } else if (action == "test") {
            auto result = environment.test();
            std::cout << result.output;
            return result.exit_code;
        } else {
            throw std::invalid_argument("action must be create, remove, build, or test");
        }
        std::cout << workspace << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ailinux-super-self: " << error.what() << '\n';
        return 1;
    }
}
