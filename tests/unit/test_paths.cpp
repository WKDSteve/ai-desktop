#include <filesystem>
#include <fstream>
#include <string>

#include "minitest.hpp"
#include "paths.hpp"

namespace fs = std::filesystem;
namespace paths = ai_desktop::paths;

TEST(dashboard_lies_next_to_the_program_not_in_the_working_directory) {
    const fs::path program = fs::path("C:/somewhere/app");
    CHECK(paths::dashboard_dir(nullptr, program) == paths::file_api_name(program / "dashboard"));
    CHECK(paths::dashboard_dir("", program) == paths::file_api_name(program / "dashboard"));
    CHECK(!paths::dashboard_dir(nullptr, program).empty());
}

TEST(dashboard_dir_from_the_environment_goes_first) {
    // Taken as the environment holds it, not through a path (whose narrow form need not be the same bytes).
    CHECK(paths::dashboard_dir("D:/own/dashboard", fs::path("C:/somewhere/app")) == "D:/own/dashboard");
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

TEST(a_folder_with_an_umlaut_is_named_so_that_narrow_file_calls_find_it) {
    // A profile like C:\Users\Juergen: the program lies below it, and cpp-httplib opens the dashboard by narrow
    // name. The UTF-8 bytes of the name are not what the narrow file calls of Windows read.
    const fs::path base = minitest::make_temp_dir("paths");
    const fs::path dir = base / fs::path(L"app-\u00fc");
    std::error_code ec;
    fs::create_directories(dir / "dashboard", ec);
    CHECK(!ec);
    {
        std::ofstream(dir / "dashboard" / "index.html") << "ok";
    }
    const std::string name = paths::dashboard_dir(nullptr, dir);
    CHECK(!name.empty());
    std::ifstream in(name + "/index.html");
    std::string text;
    in >> text;
    CHECK(text == "ok");
    fs::remove_all(base, ec);
}
