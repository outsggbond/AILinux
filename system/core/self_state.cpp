#include "self_state.hpp"

namespace ailinux {
namespace {

template <typename State>
StateTransitionResult result(State from, State to, bool accepted) {
    const auto from_name = std::string(to_string(from));
    const auto to_name = std::string(to_string(to));
    if (!accepted) {
        return {false, from_name, to_name,
                "transition from " + from_name + " to " + to_name + " is not allowed"};
    }
    return {true, from_name, to_name,
            "transitioned from " + from_name + " to " + to_name};
}

bool allowed(BaseState from, BaseState to) noexcept {
    switch (from) {
        case BaseState::Bootstrap:
            return to == BaseState::Ready || to == BaseState::Recovery ||
                   to == BaseState::Failed;
        case BaseState::Ready:
            return to == BaseState::Recovery || to == BaseState::Failed;
        case BaseState::Recovery:
            return to == BaseState::Ready || to == BaseState::Failed;
        case BaseState::Failed:
            return to == BaseState::Recovery;
    }
    return false;
}

bool allowed(RealState from, RealState to) noexcept {
    switch (from) {
        case RealState::Offline:
            return to == RealState::Booting || to == RealState::Recovery;
        case RealState::Booting:
            return to == RealState::Active || to == RealState::Degraded ||
                   to == RealState::Recovery || to == RealState::Failed;
        case RealState::Active:
            return to == RealState::Degraded || to == RealState::Recovery ||
                   to == RealState::Failed;
        case RealState::Degraded:
            return to == RealState::Active || to == RealState::Recovery ||
                   to == RealState::Failed;
        case RealState::Recovery:
            return to == RealState::Booting || to == RealState::Failed;
        case RealState::Failed:
            return to == RealState::Recovery;
    }
    return false;
}

bool allowed(SuperState from, SuperState to) noexcept {
    switch (from) {
        case SuperState::Inactive:
            return to == SuperState::Active;
        case SuperState::Active:
            return to == SuperState::Testing || to == SuperState::Inactive ||
                   to == SuperState::Failed;
        case SuperState::Testing:
            return to == SuperState::Ready || to == SuperState::Active ||
                   to == SuperState::Inactive || to == SuperState::Failed;
        case SuperState::Ready:
            return to == SuperState::Active || to == SuperState::Inactive ||
                   to == SuperState::Failed;
        case SuperState::Failed:
            return to == SuperState::Inactive;
    }
    return false;
}

bool allowed(DeploymentState from, DeploymentState to) noexcept {
    switch (from) {
        case DeploymentState::Idle:
            return to == DeploymentState::Deploying;
        case DeploymentState::Deploying:
            return to == DeploymentState::Idle || to == DeploymentState::Rollback;
        case DeploymentState::Rollback:
            return to == DeploymentState::Idle;
    }
    return false;
}

}  // namespace

SelfStateManager::SelfStateManager(SelfStateSnapshot initial) noexcept
    : state_(initial) {}

std::string_view to_string(BaseState state) noexcept {
    switch (state) {
        case BaseState::Bootstrap:
            return "BOOTSTRAP";
        case BaseState::Ready:
            return "BASE_READY";
        case BaseState::Recovery:
            return "RECOVERY";
        case BaseState::Failed:
            return "FAILED";
    }
    return "UNKNOWN";
}

std::string_view to_string(RealState state) noexcept {
    switch (state) {
        case RealState::Offline:
            return "REAL_OFFLINE";
        case RealState::Booting:
            return "REAL_BOOTING";
        case RealState::Active:
            return "REAL_ACTIVE";
        case RealState::Degraded:
            return "DEGRADED";
        case RealState::Recovery:
            return "REAL_RECOVERY";
        case RealState::Failed:
            return "REAL_FAILED";
    }
    return "UNKNOWN";
}

std::string_view to_string(SuperState state) noexcept {
    switch (state) {
        case SuperState::Inactive:
            return "SUPER_INACTIVE";
        case SuperState::Active:
            return "SUPER_ACTIVE";
        case SuperState::Testing:
            return "SUPER_TESTING";
        case SuperState::Ready:
            return "SUPER_READY";
        case SuperState::Failed:
            return "SUPER_FAILED";
    }
    return "UNKNOWN";
}

std::string_view to_string(DeploymentState state) noexcept {
    switch (state) {
        case DeploymentState::Idle:
            return "DEPLOYMENT_IDLE";
        case DeploymentState::Deploying:
            return "DEPLOYING";
        case DeploymentState::Rollback:
            return "ROLLBACK";
    }
    return "UNKNOWN";
}

SelfStateSnapshot SelfStateManager::snapshot() const noexcept {
    return state_;
}

StateTransitionResult SelfStateManager::transition_base(BaseState next) {
    auto transition = result(state_.base, next, allowed(state_.base, next));
    if (transition.accepted) {
        state_.base = next;
    }
    return transition;
}

StateTransitionResult SelfStateManager::transition_real(RealState next) {
    const bool base_ready = state_.base == BaseState::Ready;
    const bool base_recovering = state_.base == BaseState::Recovery;
    const bool environment_ready =
        next != RealState::Booting || base_ready;
    const bool recovery_available =
        next != RealState::Recovery || base_ready || base_recovering;
    const bool accepted = allowed(state_.real, next) && environment_ready &&
                          recovery_available;
    auto transition = result(state_.real, next, accepted);
    if (transition.accepted) {
        state_.real = next;
    } else if (allowed(state_.real, next) && !environment_ready) {
        transition.reason = "REAL_BOOTING requires BASE_READY";
    } else if (allowed(state_.real, next) && !recovery_available) {
        transition.reason = "REAL_RECOVERY requires a ready or recovering Base Self";
    }
    return transition;
}

StateTransitionResult SelfStateManager::transition_super(SuperState next) {
    const bool real_active = state_.real == RealState::Active;
    const bool requires_real =
        next == SuperState::Active || next == SuperState::Testing ||
        next == SuperState::Ready;
    const bool accepted = allowed(state_.super, next) &&
                          (!requires_real || real_active);
    auto transition = result(state_.super, next, accepted);
    if (transition.accepted) {
        state_.super = next;
    } else if (allowed(state_.super, next) && requires_real) {
        transition.reason = "Super Self work requires REAL_ACTIVE";
    }
    return transition;
}

StateTransitionResult SelfStateManager::transition_deployment(DeploymentState next) {
    const bool release_validated =
        state_.real == RealState::Active && state_.super == SuperState::Ready;
    const bool accepted = allowed(state_.deployment, next) &&
                          (next != DeploymentState::Deploying || release_validated);
    auto transition = result(state_.deployment, next, accepted);
    if (transition.accepted) {
        state_.deployment = next;
    } else if (allowed(state_.deployment, next) && !release_validated) {
        transition.reason = "DEPLOYING requires REAL_ACTIVE and SUPER_READY";
    }
    return transition;
}

}  // namespace ailinux
