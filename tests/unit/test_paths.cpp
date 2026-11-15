#include <filesystem>

#include "minitest.hpp"
#include "paths.hpp"

namespace fs = std::filesystem;
namespace paths = ai_desktop::paths;

TEST(dashboard_lies_next_to_the_program_not_in_the_working_directory) {
    const fs::path program = fs::path("C:/somewhere/app");
    CHECK(paths::dashboard_dir(nullptr, program) == program / "dashboard");
    CHECK(paths::dashboard_dir("", program) == program / "dashboard");
}

TEST(dashboard_dir_from_the_environment_goes_first) {
    CHECK(paths::dashboard_dir("D:/own/dashboard", fs::path("C:/somewhere/app")) == fs::path("D:/own/dashboard"));
}

TEST(dashboard_dir_is_empty_without_a_program_folder) {
    // Never the working directory: a server that cannot tell where it lies says so.
    CHECK(paths::dashboard_dir(nullptr, fs::path()).empty());
}

TEST(exe_dir_is_the_folder_of_the_test_program) {
    const fs::path dir = paths::exe_dir();
    CHECK(!dir.empty());
    CHECK(dir.is_absolute());
    std::error_code ec;
    CHECK(fs::is_directory(dir, ec));
#ifdef _WIN32
    CHECK(fs::exists(dir / "ai_desktop_tests.exe", ec));
#else
    CHECK(fs::exists(dir / "ai_desktop_tests", ec));
#endif
}
