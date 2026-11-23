cmake_minimum_required(VERSION 3.20)
project(ai-desktop CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Header-only third-party deps, vendored into third_party/ (gitignored; scripts/setup.ps1 / setup.sh fetch them).
add_library(third_party_headers INTERFACE)
target_include_directories(third_party_headers INTERFACE third_party)

# Logic that needs no HTTP stack (settings store, secret store, model registry),
# kept in its own library so the unit tests can link it without OpenSSL or
# cpp-httplib.
add_library(ai_desktop_core STATIC
    src/agent.cpp
    src/catalog_parse.cpp
    src/cli.cpp
    src/ci_status.cpp
    src/config.cpp
    src/control_secret.cpp
    src/family.cpp
    src/jobs.cpp
    src/models.cpp
    src/paths.cpp
    src/process.cpp
    src/providers.cpp
    src/review.cpp
    src/draft.cpp
    src/summary.cpp
    src/secrets.cpp
    src/session.cpp
    src/usage.cpp
    src/worktree.cpp
)
target_include_directories(ai_desktop_core PUBLIC src)
target_link_libraries(ai_desktop_core PUBLIC third_party_headers)
if(WIN32)
    # Credential Manager (secrets.cpp), the rights of control.secret (advapi32) and its random bytes (bcrypt)
    target_link_libraries(ai_desktop_core PUBLIC advapi32 bcrypt)
else()
    # process.cpp: the threads that feed and read a child
    find_package(Threads REQUIRED)
    target_link_libraries(ai_desktop_core PUBLIC Threads::Threads)
endif()

add_executable(ai-desktop
    src/main.cpp
    src/api.cpp
    src/catalog.cpp
    src/cli_run.cpp
    src/foreground.cpp
    src/jobs_api.cpp
    src/ollama_client.cpp
    src/openai_client.cpp
    src/anthropic_client.cpp
    src/gemini_client.cpp
)
target_include_directories(ai-desktop PRIVATE src)
# The version the server reports in hub.json: the release and the rolling build pass the number of the
# installer (scripts/setup.ps1 -Version), a build by hand says 0.0.0.
set(AI_DESKTOP_VERSION "0.0.0" CACHE STRING "The version of ai-desktop (X.Y.Z)")
target_compile_definitions(ai-desktop PRIVATE AI_DESKTOP_VERSION="${AI_DESKTOP_VERSION}")
target_link_libraries(ai-desktop PRIVATE ai_desktop_core third_party_headers)

# The cloud ModelRouter backends (openai/anthropic/gemini) talk to real
# HTTPS endpoints, unlike ollama_client's localhost-only HTTP -- cpp-httplib
# needs CPPHTTPLIB_OPENSSL_SUPPORT plus OpenSSL to dispatch an "https://"
# httplib::Client to its SSL-capable code path. Required, not optional: the
# router falls back to Ollama when a cloud model prefix's key is missing,
# but the binary must still link successfully regardless of which keys are
# set at runtime.
find_package(OpenSSL REQUIRED)
target_compile_definitions(ai-desktop PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(ai-desktop PRIVATE OpenSSL::SSL OpenSSL::Crypto)

if(WIN32)
    target_link_libraries(ai-desktop PRIVATE ws2_32 crypt32 shell32 ole32) # shell32/ole32: foreground.cpp
else()
    find_package(Threads REQUIRED)
    target_link_libraries(ai-desktop PRIVATE Threads::Threads)
endif()

# Unit tests (plain asserts, no framework): `ctest --test-dir build`. The HTTP
# API is covered separately by the Node tests under tests/.
option(AI_DESKTOP_BUILD_TESTS "Build the unit tests" ON)
if(AI_DESKTOP_BUILD_TESTS)
    enable_testing()
    add_executable(ai_desktop_tests
        tests/unit/test_main.cpp
        tests/unit/test_agent.cpp
        tests/unit/test_catalog_parse.cpp
        tests/unit/test_cli.cpp
        tests/unit/test_ci_status.cpp
        tests/unit/test_config.cpp
        tests/unit/test_control_secret.cpp
        tests/unit/test_family.cpp
        tests/unit/test_jobs.cpp
        tests/unit/test_models.cpp
        tests/unit/test_paths.cpp
        tests/unit/test_process.cpp
        tests/unit/test_review.cpp
        tests/unit/test_draft.cpp
        tests/unit/test_summary.cpp
        tests/unit/test_providers.cpp
        tests/unit/test_secrets.cpp
        tests/unit/test_session.cpp
        tests/unit/test_usage.cpp
        tests/unit/test_worktree.cpp
    )
    target_link_libraries(ai_desktop_tests PRIVATE ai_desktop_core)
    add_test(NAME ai_desktop_unit COMMAND ai_desktop_tests)

    # Stand-ins for Claude Code and the tasks command (tests/doubles/): the API and browser tests run a job of the
    # agent model against them. Built next to ai-desktop; never installed.
    add_executable(fake_claude tests/doubles/fake_claude.cpp)
    target_link_libraries(fake_claude PRIVATE third_party_headers)
    add_executable(fake_tasks tests/doubles/fake_tasks.cpp)
    target_link_libraries(fake_tasks PRIVATE third_party_headers)
    # The unit tests start fake_tasks as a child (tests/unit/test_process.cpp).
    add_dependencies(ai_desktop_tests fake_tasks)
    target_compile_definitions(ai_desktop_tests PRIVATE AI_DESKTOP_FAKE_TASKS="$<TARGET_FILE:fake_tasks>")
endif()

# The server serves the "dashboard" folder next to its own binary (src/paths.cpp;
# AI_DESKTOP_DASHBOARD_DIR goes first), whatever the working directory. The build
# copies it next to the built binary, so `build/ai-desktop` serves the dashboard of
# the last build (set AI_DESKTOP_DASHBOARD_DIR to the repo's folder to edit it live).
# A target of its own that runs on every build: a POST_BUILD step of ai-desktop ran
# only when the binary was linked again, so a build after a change of the dashboard
# alone left the old one next to the binary.
add_custom_target(ai-desktop-dashboard ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/dashboard $<TARGET_FILE_DIR:ai-desktop>/dashboard
    COMMENT "Copying the dashboard next to ai-desktop")
add_dependencies(ai-desktop-dashboard ai-desktop)

# Flat install layout: the binary and the dashboard folder side by side.
# scripts/setup.ps1 and scripts/setup.sh install into <prefix>/app and write a
# launcher that keeps <prefix>/app (on Windows with the runtime DLLs) off PATH.
install(TARGETS ai-desktop RUNTIME DESTINATION .)
install(DIRECTORY dashboard DESTINATION .)

if(AI_DESKTOP_BUILD_TESTS)
    # The copy next to the binary is the dashboard of the sources (also after a
    # build that changed only the dashboard).
    add_test(NAME dashboard_copy
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/dashboard
                -DDST=$<TARGET_FILE_DIR:ai-desktop>/dashboard
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check-dashboard-copy.cmake)
endif()
