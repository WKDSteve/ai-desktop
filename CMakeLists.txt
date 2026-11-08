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
    src/catalog_parse.cpp
    src/config.cpp
    src/models.cpp
    src/providers.cpp
    src/secrets.cpp
    src/usage.cpp
)
target_include_directories(ai_desktop_core PUBLIC src)
target_link_libraries(ai_desktop_core PUBLIC third_party_headers)
if(WIN32)
    target_link_libraries(ai_desktop_core PUBLIC advapi32) # Credential Manager (secrets.cpp)
endif()

add_executable(ai-desktop
    src/main.cpp
    src/api.cpp
    src/catalog.cpp
    src/ollama_client.cpp
    src/openai_client.cpp
    src/anthropic_client.cpp
    src/gemini_client.cpp
)
target_include_directories(ai-desktop PRIVATE src)
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
    target_link_libraries(ai-desktop PRIVATE ws2_32 crypt32)
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
        tests/unit/test_catalog_parse.cpp
        tests/unit/test_config.cpp
        tests/unit/test_models.cpp
        tests/unit/test_providers.cpp
        tests/unit/test_secrets.cpp
        tests/unit/test_usage.cpp
    )
    target_link_libraries(ai_desktop_tests PRIVATE ai_desktop_core)
    add_test(NAME ai_desktop_unit COMMAND ai_desktop_tests)
endif()

# Flat install layout: the server mounts "./dashboard" relative to its working
# directory, so the binary and the dashboard folder must sit side by side and
# the server must be started from there. scripts/setup.ps1 and scripts/setup.sh
# install into <prefix>/app and write a launcher that does the cd.
install(TARGETS ai-desktop RUNTIME DESTINATION .)
install(DIRECTORY dashboard DESTINATION .)
