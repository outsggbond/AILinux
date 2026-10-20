cmake_minimum_required(VERSION 3.20)

project(AILinux
    VERSION 0.1.0
    DESCRIPTION "AILinux Operating System Project"
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ailinuxctl
    system/tools/ailinuxctl/main.cpp
)

target_compile_options(ailinuxctl PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
