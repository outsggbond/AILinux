# Current System Audit

Audit date: 2026-11-19

## Scope and evidence

This audit distinguishes the AILinux repository from the Debian host on which
it was inspected. Host facts were read from `/etc/os-release`, `/proc`,
`/boot`, `findmnt`, `lsblk`, the active kernel configuration, and installed
system commands. Repository facts were read from tracked files, Git metadata,
the CMake project, and the Linux submodule.

The source baseline is commit `fb35694` (`getinthelinux`) on `main`. At task
start, `README.md` already contained uncommitted project requirements and
`.gitmodules` was untracked. To preserve that work, development moved to the
`ai-native/phase-0-audit` branch without resetting or rewriting those files.
The branch is not a clean snapshot of the baseline; no commit or filesystem
snapshot was created because the pre-existing working-tree changes were not
owned by this implementation.

## Findings

| Area | Verified finding |
|---|---|
| Project type | A small CMake/C++ project; it is not currently a Linux distribution build system. |
| Project source | The only original application source is `system/tools/ailinuxctl/main.cpp`. |
| Kernel source | `kernel/linux` is a Git submodule at Linux `v6.12.118`, from the upstream stable repository. There are no AILinux kernel patches, kernel configuration files, or kernel build scripts in the project. |
| Host distribution | The inspected host runs Debian GNU/Linux 13 (trixie). This does not establish that AILinux itself is a Debian-derived distribution. |
| Host kernel | The running host kernel is `6.12.113+deb13-amd64`; this differs from the checked-out kernel source version. |
| Boot and init | The host has GRUB files under `/boot/grub`, systemd is PID 1, and two Debian kernel images are present. The repository does not configure or install either bootloader or init system. |
| Host package management | `apt` and `dpkg` are installed on the host. The project contains no package-manager adapter. |
| Host root filesystem | `/` is an ext4 filesystem on `/dev/sda1`; this is host state, not an AILinux image layout. |
| Kernel facilities | The active host config enables cgroups, namespaces, seccomp, BPF, EFI and initramfs support. These are host-kernel capabilities and have not been verified against the submodule's build configuration or a target image. |
| Security and permissions | The repository has no daemon, privilege broker, policy engine, service unit, or credential store. The existing CLI is an unprivileged read-only information program. |
| Data and recovery | No AILinux release manifest, verified snapshot, A/B root filesystem, watchdog, recovery image, or rollback implementation exists. |
| Tests | There was no project test target at the source baseline. This branch adds two CTest targets for self-state policy and persistent state journaling. |
| Build | CMake 3.31.6, Ninja 1.12.1, GCC 14.2, and OpenSSL Crypto 3.5.7 are present. CMake builds `ailinuxctl`, `ailinux_core`, and their tests; it does not build an OS image. |

## Conclusions

The current checkout is a prototype repository, not a bootable or installable
AILinux distribution. The Debian host environment must not be confused with
project-owned OS code. The upstream kernel is source material only; no kernel
modification is justified for the currently implemented features.

The safe first implementation boundary is unprivileged user-space code and
deterministic tests. System mutation, AI provider access, secrets, and automatic
deployment remain out of scope until their permission and recovery foundations
are implemented and verified.
