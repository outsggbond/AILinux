#pragma once

#include "council.hpp"
#include "reputation.hpp"
#include "runtime.hpp"

#include <stdexcept>
#include <string>

namespace ailinux::ai {

class CouncilManagerError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class CouncilManager {
public:
    CouncilManager(CouncilWorkflow& workflow, AgentReputationStore& reputation,
                   AgentScheduler& scheduler);

    void deliberate(const std::string& session_id);
    void inspect_execution(const std::string& session_id);

private:
    CouncilWorkflow& workflow_;
    AgentReputationStore& reputation_;
    AgentScheduler& scheduler_;
};

} // namespace ailinux::ai
