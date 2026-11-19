# Language Boundaries

AILinux-owned user-space runtime modules use C++20, including system
interfaces, AI providers, Agent reputation and governance, scheduling,
health/process monitoring, and tool dispatch. This follows the user's explicit requirement that project
runtime code be implemented in C++ rather than Python.

The upstream Linux kernel remains the upstream C project. Its source, scripts,
and build tools are not AILinux-owned runtime modules and are not rewritten to
meet the C++ user-space requirement. The kernel is tracked as a submodule and
has no AILinux kernel patches or project kernel configuration in this slice.

The repository has no AILinux-owned Python runtime module or Python test
dependency. Native libraries used by the current C++ modules are OpenSSL,
SQLite, libcurl, and nlohmann-json. The current tool broker is an
unauthenticated in-process foundation; no privileged system-operation broker
or resident AI core service exists yet. The read-only health monitor has a
separate installable systemd service.

System operations must not be added as arbitrary shell execution. The current
in-process C++ tool broker is an unauthenticated policy foundation, not a
privileged security boundary. Any real privileged operation requires a
separately designed, narrowly authorized, privilege-separated C++ broker, with
independent review and explicit audit records.
