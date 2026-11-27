#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cli.hpp"
#include "minitest.hpp"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
namespace cli = ai_desktop::cli;

namespace {

std::string read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_all(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

}  // namespace

TEST(no_argument_and_only_port_are_the_server) {
    CHECK(cli::parse({}).command == cli::Command::Serve);
    CHECK(cli::parse({"--port", "9000"}).command == cli::Command::Serve);
    CHECK(cli::parse({"--port=9000"}).command == cli::Command::Serve);
}

TEST(the_commands_and_their_flags) {
    CHECK(cli::parse({"start"}).command == cli::Command::Start);
    CHECK(!cli::parse({"start"}).open);
    CHECK(cli::parse({"start", "--open"}).open);
    CHECK_EQ(cli::parse({"start", "--port", "9001"}).port, std::string("9001"));
    CHECK_EQ(cli::parse({"start", "--port=9002"}).port, std::string("9002"));
    CHECK(cli::parse({"stop"}).command == cli::Command::Stop);
    CHECK(cli::parse({"quit"}).command == cli::Command::Stop);
    CHECK(cli::parse({"status"}).command == cli::Command::Status);
    CHECK(cli::parse({"status", "--json"}).json);
    CHECK(cli::parse({"open"}).command == cli::Command::Open);
    CHECK(cli::parse({"--version"}).command == cli::Command::Version);
    CHECK(cli::parse({"-V"}).command == cli::Command::Version);
    CHECK(cli::parse({"version"}).command == cli::Command::Version);
    CHECK(cli::parse({"--help"}).command == cli::Command::Help);
    CHECK(cli::parse({"-h"}).command == cli::Command::Help);
    CHECK(cli::parse({"help"}).command == cli::Command::Help);
    CHECK(cli::parse({"start", "--help"}).command == cli::Command::Help);
    CHECK(cli::parse({"cli-install"}).command == cli::Command::CliInstall);
    CHECK_EQ(cli::parse({"cli-install", "--program", "C:/x/ai-desktop.exe"}).program, std::string("C:/x/ai-desktop.exe"));
    CHECK(cli::parse({"cli-uninstall"}).command == cli::Command::CliUninstall);
}

TEST(wrong_use_is_invalid_with_a_reason) {
    for (const auto& args : std::vector<std::vector<std::string>>{{"frobnicate"},
                                                                  {"stop", "--now"},
                                                                  {"status", "--open"},
                                                                  {"start", "--json"},
                                                                  {"start", "--port"},
                                                                  {"--version", "x"},
                                                                  {"--bogus"}}) {
        const cli::Parsed parsed = cli::parse(args);
        CHECK(parsed.command == cli::Command::Invalid);
        CHECK(!parsed.error.empty());
    }
}

TEST(status_json_is_one_line_with_the_running_facts) {
    cli::Status status;
    status.running = true;
    status.version = "0.1.0";
    status.port = 8080;
    status.pid = 42;
    status.instance = "ab";
    status.config_dir = "C:/cfg";
    const auto doc = nlohmann::json::parse(cli::status_json(status));
    CHECK(doc["running"] == true);
    CHECK(doc["port"] == 8080);
    CHECK(doc["pid"] == 42);
    CHECK(doc["url"] == "http://127.0.0.1:8080/");
    CHECK(cli::status_json(status).find('\n') == std::string::npos);

    cli::Status stopped;
    stopped.version = "0.1.0";
    const auto off = nlohmann::json::parse(cli::status_json(stopped));
    CHECK(off["running"] == false);
    CHECK(!off.contains("port"));
    CHECK(cli::status_text(stopped).find("not running") != std::string::npos);
    CHECK(cli::status_text(status).find("running") != std::string::npos);
}

TEST(the_cmd_starter_names_the_program_below_localappdata_and_hands_the_exit_code_back) {
    std::string text, error;
    CHECK(cli::cmd_starter("C:\\Users\\me\\AppData\\Local\\Programs\\ai-desktop\\app\\ai-desktop.exe",
                           "C:\\Users\\me\\AppData\\Local", &text, &error));
    CHECK(text.find("set \"program=%LOCALAPPDATA%\\Programs\\ai-desktop\\app\\ai-desktop.exe\"\r\n") !=
          std::string::npos);
    CHECK(text.find("appkit starter") != std::string::npos);
    CHECK(text.find("\"%program%\" %*\r\nexit /b %ERRORLEVEL%\r\n") != std::string::npos);
    CHECK(text.find("exit /b 3") != std::string::npos);
    CHECK(text.find("\r\n") != std::string::npos && text.find("\n") == text.find("\r\n") + 1);
}

TEST(the_cmd_starter_never_holds_a_verbatim_path_or_a_char_cmd_cannot_read) {
    std::string text, error;
    CHECK(cli::cmd_starter("\\\\?\\C:\\Apps\\ai-desktop.exe", "", &text, &error));
    CHECK(text.find("\\\\?\\") == std::string::npos);
    CHECK(text.find("set \"program=C:\\Apps\\ai-desktop.exe\"") != std::string::npos);
    CHECK(cli::cmd_starter("C:\\100%\\ai-desktop.exe", "", &text, &error));
    CHECK(text.find("C:\\100%%\\ai-desktop.exe") != std::string::npos);
    CHECK(!cli::cmd_starter("C:\\Bj\xC3\xB6rn\\ai-desktop.exe", "", &text, &error));
    CHECK(!error.empty());
    CHECK(!cli::cmd_starter("C:\\a\"b\\ai-desktop.exe", "", &text, &error));
}

TEST(the_sh_starter_quotes_the_path_and_ends_3_when_the_program_is_gone) {
    std::string text, error;
    CHECK(cli::sh_starter("/opt/it's/ai-desktop", &text, &error));
    CHECK(text.rfind("#!/bin/sh\n", 0) == 0);
    CHECK(text.find("appkit starter") != std::string::npos);
    CHECK(text.find("program='/opt/it'\\''s/ai-desktop'\n") != std::string::npos);
    CHECK(text.find("exit 3\n") != std::string::npos);
    CHECK(text.find("exec \"$program\" \"$@\"\n") != std::string::npos);
    CHECK(text.find('\r') == std::string::npos);
}

TEST(install_writes_the_starters_and_uninstall_takes_only_them_and_the_empty_folder) {
    const fs::path dir = minitest::make_temp_dir("cli-starters") / "bin";
    CHECK_EQ(cli::install_starters(dir, "ai-desktop", "C:/x/ai-desktop.exe", ""), std::string());
    CHECK(fs::exists(dir / "ai-desktop"));
#ifdef _WIN32
    CHECK(fs::exists(dir / "ai-desktop.cmd"));
    CHECK(read_all(dir / "ai-desktop.cmd").find("appkit starter") != std::string::npos);
#endif
    // Again: replaced, not an error.
    CHECK_EQ(cli::install_starters(dir, "ai-desktop", "C:/y/ai-desktop.exe", ""), std::string());
    CHECK(read_all(dir / "ai-desktop").find("C:/y/ai-desktop.exe") != std::string::npos);

    // Another app's starter stays, and so does the folder.
    write_all(dir / "tasks-desktop", "#!/bin/sh\n# appkit starter of tasks-desktop\n");
    bool removed = true;
    CHECK_EQ(cli::uninstall_starters(dir, "ai-desktop", &removed), std::string());
    CHECK(!removed);
    CHECK(!fs::exists(dir / "ai-desktop"));
    CHECK(!fs::exists(dir / "ai-desktop.cmd"));
    CHECK(fs::exists(dir / "tasks-desktop"));

    // The last one takes the folder with it.
    fs::remove(dir / "tasks-desktop");
    CHECK_EQ(cli::install_starters(dir, "ai-desktop", "C:/x/ai-desktop.exe", ""), std::string());
    CHECK_EQ(cli::uninstall_starters(dir, "ai-desktop", &removed), std::string());
    CHECK(removed);
    CHECK(!fs::exists(dir));
    minitest::remove_tree(dir.parent_path());
}

TEST(a_file_of_that_name_that_is_no_starter_is_never_overwritten_or_removed) {
    const fs::path dir = minitest::make_temp_dir("cli-foreign") / "bin";
    fs::create_directories(dir);
    write_all(dir / "ai-desktop", "my own script\n");
    CHECK(!cli::install_starters(dir, "ai-desktop", "C:/x/ai-desktop.exe", "").empty());
    CHECK_EQ(read_all(dir / "ai-desktop"), std::string("my own script\n"));
    bool removed = true;
    CHECK_EQ(cli::uninstall_starters(dir, "ai-desktop", &removed), std::string());
    CHECK(fs::exists(dir / "ai-desktop"));
    CHECK(!removed);
    minitest::remove_tree(dir.parent_path());
}

TEST(the_starter_folder_follows_APPKIT_FAMILY_DIR) {
    minitest::set_env("APPKIT_FAMILY_DIR", "C:/fam");
    CHECK(cli::starter_dir() == fs::path("C:/fam") / "bin");
    minitest::set_env("APPKIT_FAMILY_DIR", "");
}

// People always get the visible console of the server; only the switch of the tests hides it.
TEST(the_server_console_is_visible_unless_the_tests_switch_it_off) {
    CHECK(cli::server_console(nullptr) == cli::ServerConsole::Visible);
    CHECK(cli::server_console("") == cli::ServerConsole::Visible);
    CHECK(cli::server_console("0") == cli::ServerConsole::Visible);
    CHECK(cli::server_console("1") == cli::ServerConsole::None);
    CHECK(cli::server_console("yes") == cli::ServerConsole::None);
}
