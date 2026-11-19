#include "self_state.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename State, std::size_t N, typename Transition>
void check_transition_graph(
    const std::array<State, N>& states,
    const auto& allowed_transitions,
    Transition transition) {
    for (const auto from : states) {
        for (const auto to : states) {
            bool expected = false;
            for (const auto& edge : allowed_transitions) {
                if (edge.first == from && edge.second == to) {
                    expected = true;
                    break;
                }
            }
            check(transition(from, to) == expected,
                  "transition graph matches its explicit edge list");
        }
    }
}

}  // namespace

int main() {
    using namespace ailinux;

    constexpr std::array base_states{
        BaseState::Bootstrap, BaseState::Ready, BaseState::Recovery, BaseState::Failed};
    constexpr std::array base_edges{
        std::pair{BaseState::Bootstrap, BaseState::Ready},
        std::pair{BaseState::Bootstrap, BaseState::Recovery},
        std::pair{BaseState::Bootstrap, BaseState::Failed},
        std::pair{BaseState::Ready, BaseState::Recovery},
        std::pair{BaseState::Ready, BaseState::Failed},
        std::pair{BaseState::Recovery, BaseState::Ready},
        std::pair{BaseState::Recovery, BaseState::Failed},
        std::pair{BaseState::Failed, BaseState::Recovery},
    };
    check_transition_graph(base_states, base_edges, [](BaseState from, BaseState to) {
        SelfStateManager manager;
        if (from == BaseState::Ready) {
            manager.transition_base(BaseState::Ready);
        } else if (from == BaseState::Recovery) {
            manager.transition_base(BaseState::Recovery);
        } else if (from == BaseState::Failed) {
            manager.transition_base(BaseState::Failed);
        }
        const auto result = manager.transition_base(to);
        return result.accepted;
    });

    constexpr std::array real_states{
        RealState::Offline, RealState::Booting, RealState::Active,
        RealState::Degraded, RealState::Recovery, RealState::Failed};
    constexpr std::array real_edges{
        std::pair{RealState::Offline, RealState::Booting},
        std::pair{RealState::Offline, RealState::Recovery},
        std::pair{RealState::Booting, RealState::Active},
        std::pair{RealState::Booting, RealState::Degraded},
        std::pair{RealState::Booting, RealState::Recovery},
        std::pair{RealState::Booting, RealState::Failed},
        std::pair{RealState::Active, RealState::Degraded},
        std::pair{RealState::Active, RealState::Recovery},
        std::pair{RealState::Active, RealState::Failed},
        std::pair{RealState::Degraded, RealState::Active},
        std::pair{RealState::Degraded, RealState::Recovery},
        std::pair{RealState::Degraded, RealState::Failed},
        std::pair{RealState::Recovery, RealState::Booting},
        std::pair{RealState::Recovery, RealState::Failed},
        std::pair{RealState::Failed, RealState::Recovery},
    };
    check_transition_graph(real_states, real_edges, [](RealState from, RealState to) {
        SelfStateManager manager;
        manager.transition_base(BaseState::Ready);
        if (from == RealState::Booting || from == RealState::Active ||
            from == RealState::Degraded || from == RealState::Failed) {
            manager.transition_real(RealState::Booting);
        }
        if (from == RealState::Active || from == RealState::Degraded) {
            manager.transition_real(RealState::Active);
        }
        if (from == RealState::Degraded) {
            manager.transition_real(RealState::Degraded);
        } else if (from == RealState::Recovery) {
            manager.transition_real(RealState::Recovery);
        } else if (from == RealState::Failed) {
            manager.transition_real(RealState::Failed);
        }
        return manager.transition_real(to).accepted;
    });

    constexpr std::array super_states{
        SuperState::Inactive, SuperState::Active, SuperState::Testing,
        SuperState::Ready, SuperState::Failed};
    constexpr std::array super_edges{
        std::pair{SuperState::Inactive, SuperState::Active},
        std::pair{SuperState::Active, SuperState::Testing},
        std::pair{SuperState::Active, SuperState::Inactive},
        std::pair{SuperState::Active, SuperState::Failed},
        std::pair{SuperState::Testing, SuperState::Ready},
        std::pair{SuperState::Testing, SuperState::Active},
        std::pair{SuperState::Testing, SuperState::Inactive},
        std::pair{SuperState::Testing, SuperState::Failed},
        std::pair{SuperState::Ready, SuperState::Active},
        std::pair{SuperState::Ready, SuperState::Inactive},
        std::pair{SuperState::Ready, SuperState::Failed},
        std::pair{SuperState::Failed, SuperState::Inactive},
    };
    check_transition_graph(super_states, super_edges, [](SuperState from, SuperState to) {
        SelfStateManager manager;
        manager.transition_base(BaseState::Ready);
        manager.transition_real(RealState::Booting);
        manager.transition_real(RealState::Active);
        if (from == SuperState::Active || from == SuperState::Testing ||
            from == SuperState::Ready || from == SuperState::Failed) {
            manager.transition_super(SuperState::Active);
        }
        if (from == SuperState::Testing || from == SuperState::Ready) {
            manager.transition_super(SuperState::Testing);
        }
        if (from == SuperState::Ready) {
            manager.transition_super(SuperState::Ready);
        } else if (from == SuperState::Failed) {
            manager.transition_super(SuperState::Failed);
        }
        return manager.transition_super(to).accepted;
    });

    constexpr std::array deployment_states{
        DeploymentState::Idle, DeploymentState::Deploying, DeploymentState::Rollback};
    constexpr std::array deployment_edges{
        std::pair{DeploymentState::Idle, DeploymentState::Deploying},
        std::pair{DeploymentState::Deploying, DeploymentState::Idle},
        std::pair{DeploymentState::Deploying, DeploymentState::Rollback},
        std::pair{DeploymentState::Rollback, DeploymentState::Idle},
    };

    check_transition_graph(deployment_states, deployment_edges, [](auto from, auto to) {
        SelfStateManager manager;
        manager.transition_base(BaseState::Ready);
        manager.transition_real(RealState::Booting);
        manager.transition_real(RealState::Active);
        manager.transition_super(SuperState::Active);
        manager.transition_super(SuperState::Testing);
        manager.transition_super(SuperState::Ready);
        if (from == DeploymentState::Deploying || from == DeploymentState::Rollback) {
            manager.transition_deployment(DeploymentState::Deploying);
        }
        if (from == DeploymentState::Rollback) {
            manager.transition_deployment(DeploymentState::Rollback);
        }
        return manager.transition_deployment(to).accepted;
    });

    SelfStateManager manager;
    check(manager.snapshot().base == BaseState::Bootstrap,
          "Base Self starts in BOOTSTRAP");
    check(manager.transition_base(BaseState::Ready).accepted,
          "Base Self can become ready");
    check(manager.transition_real(RealState::Booting).accepted,
          "Real Self can boot after Base Self is ready");
    check(manager.transition_real(RealState::Active).accepted,
          "Real Self can become active after boot");
    check(manager.transition_super(SuperState::Active).accepted,
          "Super Self can run alongside REAL_ACTIVE");
    check(manager.snapshot().real == RealState::Active &&
              manager.snapshot().super == SuperState::Active,
          "REAL_ACTIVE and SUPER_ACTIVE coexist");

    SelfStateManager invalid_boot;
    const auto denied_boot = invalid_boot.transition_real(RealState::Booting);
    check(!denied_boot.accepted, "Real Self cannot boot before Base Self is ready");
    check(invalid_boot.snapshot().real == RealState::Offline,
          "rejected boot transition preserves state");

    SelfStateManager invalid_deployment;
    invalid_deployment.transition_base(BaseState::Ready);
    invalid_deployment.transition_real(RealState::Booting);
    invalid_deployment.transition_real(RealState::Active);
    const auto denied_deploy =
        invalid_deployment.transition_deployment(DeploymentState::Deploying);
    check(!denied_deploy.accepted, "deployment requires a validated Super Self");
    check(invalid_deployment.snapshot().deployment == DeploymentState::Idle,
          "rejected deployment transition preserves state");

    check(to_string(BaseState::Ready) == "BASE_READY", "Base Self state name");
    check(to_string(RealState::Active) == "REAL_ACTIVE", "Real Self state name");
    check(to_string(SuperState::Testing) == "SUPER_TESTING", "Super Self state name");
    check(to_string(DeploymentState::Rollback) == "ROLLBACK", "rollback state name");

    if (failures != 0) {
        std::cerr << failures << " self-state test(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "All self-state tests passed\n";
    return EXIT_SUCCESS;
}
