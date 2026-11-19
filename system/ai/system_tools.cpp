#include "system_tools.hpp"

#include "tool_broker.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <stdexcept>

namespace ailinux::ai {

void register_system_tools(ToolBroker& broker) {
    broker.register_tool(
        {"system.get_status",
         {{"type", "object"},
          {"properties", nlohmann::json::object()},
          {"additionalProperties", false}},
         ToolRisk::read_only},
        [](const nlohmann::json&) {
            struct utsname identity {};
            if (uname(&identity) != 0) {
                throw ToolExecutionError("system_identity_unavailable");
            }
            const auto online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
            if (online_cpus <= 0) {
                throw ToolExecutionError("cpu_count_unavailable");
            }
            return nlohmann::json{
                {"sysname", identity.sysname},
                {"release", identity.release},
                {"version", identity.version},
                {"machine", identity.machine},
                {"online_cpus", online_cpus}};
        });
}

} // namespace ailinux::ai
