# Current Build Process

## Toolchain observed

- CMake 3.31.6
- Ninja 1.12.1
- GCC/G++ 14.2.0
- CMake minimum required by the project: 3.20
- C++ standard: C++20
- OpenSSL Crypto, SQLite3, libcurl, nlohmann-json, and POSIX threads

## Build and test

From the repository root:

```sh
cmake -S . -B build/cmake -G Ninja
cmake --build build/cmake
ctest --test-dir build/cmake --output-on-failure
```

The resulting `build/cmake/ailinuxctl` is a host utility. The build also
produces the `ailinux_core` and `ailinux_ai` libraries and their native C++
tests. The build does not produce a Linux kernel, root filesystem, package,
bootable image, or installer. AILinux-owned runtime modules have no Python
runtime or Python test dependency.

## Kernel source

Initialize the pinned submodule with:

```sh
git submodule update --init --recursive
```

No kernel configuration, cross-toolchain, image packaging, or kernel deployment
is defined by this project. Building or installing the kernel submodule is not
part of the project build.

## Continuous integration and release

No CI workflow, release pipeline, package definition, deployment procedure, or
automated rollback process was present during the audit. These remain future
work; a successful CMake build must not be described as a successful OS build.
