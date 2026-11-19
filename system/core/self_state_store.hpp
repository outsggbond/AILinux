#pragma once

#include "self_state.hpp"

#include <filesystem>
#include <string>

namespace ailinux {

class SelfStateStore {
public:
    explicit SelfStateStore(std::filesystem::path directory);

    SelfStateSnapshot load();
    void append(const SelfStateSnapshot& expected, const SelfStateSnapshot& next);

private:
    std::filesystem::path directory_;
};

class PersistentSelfStateManager {
public:
    explicit PersistentSelfStateManager(std::filesystem::path directory);

    SelfStateSnapshot snapshot() const noexcept;
    StateTransitionResult transition_base(BaseState next);
    StateTransitionResult transition_real(RealState next);
    StateTransitionResult transition_super(SuperState next);
    StateTransitionResult transition_deployment(DeploymentState next);

private:
    StateTransitionResult persist_candidate(SelfStateManager candidate,
                                            StateTransitionResult transition);

    SelfStateStore store_;
    SelfStateManager manager_;
};

}  // namespace ailinux
