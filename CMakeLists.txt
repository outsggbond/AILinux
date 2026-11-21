cmake_minimum_required(VERSION 3.20)

project(AILinux
    VERSION 0.1.0
    DESCRIPTION "AILinux Operating System Project"
    LANGUAGES CXX
)

include(GNUInstallDirs)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(Threads REQUIRED)

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

add_library(ailinux_core
    system/core/self_state.cpp
    system/core/self_state_store.cpp
    system/core/super_self_workspace.cpp
)

target_include_directories(ailinux_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/system/core
)

target_compile_options(ailinux_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
target_link_libraries(ailinux_core PUBLIC OpenSSL::Crypto)

add_executable(ailinux-super-self
    system/core/super_self_workspace_main.cpp
)
target_compile_options(ailinux-super-self PRIVATE
    -Wall -Wextra -Wpedantic
)
target_link_libraries(ailinux-super-self PRIVATE ailinux_core)

add_library(ailinux_ai
    system/ai/providers.cpp
    system/ai/reputation.cpp
    system/ai/runtime.cpp
    system/ai/health_monitor.cpp
    system/ai/tool_broker.cpp
    system/ai/process_monitor.cpp
    system/ai/council.cpp
    system/ai/council_manager.cpp
    system/ai/system_tools.cpp
)
target_include_directories(ailinux_ai PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/system/ai
)
target_compile_options(ailinux_ai PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
target_link_libraries(ailinux_ai
    PUBLIC CURL::libcurl SQLite::SQLite3 nlohmann_json::nlohmann_json
           OpenSSL::Crypto Threads::Threads
)

add_executable(ailinux-healthd system/ai/healthd.cpp)
target_compile_options(ailinux-healthd PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ailinux-healthd PRIVATE ailinux_ai)

configure_file(
    system/services/ailinux-healthd.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/ailinux-healthd.service
    @ONLY
)
install(TARGETS ailinux-healthd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ailinux-healthd.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)

include(CTest)

if(BUILD_TESTING)
    add_executable(self_state_test
        tests/self_state_test.cpp
    )
    target_link_libraries(self_state_test PRIVATE ailinux_core)
    target_compile_options(self_state_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    add_test(NAME self_state_test COMMAND self_state_test)

    add_executable(self_state_store_test
        tests/self_state_store_test.cpp
    )
    target_link_libraries(self_state_store_test PRIVATE ailinux_core)
    target_compile_options(self_state_store_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    add_test(NAME self_state_store_test COMMAND self_state_store_test)
    add_executable(ai_system_test tests/ai_system_test.cpp)
    target_link_libraries(ai_system_test PRIVATE ailinux_ai)
    target_compile_options(ai_system_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME ai_system_test COMMAND ai_system_test)

    add_executable(super_self_workspace_test
        tests/super_self_workspace_test.cpp
    )
    target_compile_definitions(super_self_workspace_test PRIVATE
        AILINUX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(super_self_workspace_test PRIVATE ailinux_core)
    target_compile_options(super_self_workspace_test PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME super_self_workspace_test COMMAND super_self_workspace_test)

    add_test(NAME healthd_once COMMAND ailinux-healthd --once)
    set_tests_properties(healthd_once PROPERTIES
        PASS_REGULAR_EXPRESSION "\"metrics\":.*\"overall\":.*\"schema_version\":1"
    )
endif()
