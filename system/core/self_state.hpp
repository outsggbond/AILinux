#pragma once

#include <string>
#include <string_view>

namespace ailinux {

enum class BaseState {
    Bootstrap,
    Ready,
    Recovery,
    Failed,
};

enum class RealState {
    Offline,
    Booting,
    Active,
    Degraded,
    Recovery,
    Failed,
};

enum class SuperState {
    Inactive,
    Active,
    Testing,
    Ready,
    Failed,
};

enum class DeploymentState {
    Idle,
    Deploying,
    Rollback,
};

std::string_view to_string(BaseState state) noexcept;
std::string_view to_string(RealState state) noexcept;
std::string_view to_string(SuperState state) noexcept;
std::string_view to_string(DeploymentState state) noexcept;

struct StateTransitionResult {
    bool accepted;
    std::string from;
    std::string to;
    std::string reason;
};

struct SelfStateSnapshot {
    BaseState base = BaseState::Bootstrap;
    RealState real = RealState::Offline;
    SuperState super = SuperState::Inactive;
    DeploymentState deployment = DeploymentState::Idle;

    bool operator==(const SelfStateSnapshot&) const = default;
};

class SelfStateManager {
public:
    SelfStateManager() = default;
    explicit SelfStateManager(SelfStateSnapshot initial) noexcept;

    SelfStateSnapshot snapshot() const noexcept;

    StateTransitionResult transition_base(BaseState next);
    StateTransitionResult transition_real(RealState next);
    StateTransitionResult transition_super(SuperState next);
    StateTransitionResult transition_deployment(DeploymentState next);

private:
    SelfStateSnapshot state_;
};

}  // namespace ailinux
