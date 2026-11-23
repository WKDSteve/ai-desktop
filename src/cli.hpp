#pragma once

// The command in the terminal: `ai-desktop start [--open]`, `stop` (`quit`), `status [--json]`, `open`, `--version`,
// `--help`, the same set the other apps of the family have (docs/FAMILY.md of appkit, "The command in the terminal").
// Without an argument -- or with only `--port N` -- ai-desktop is the server, as it always was.
//
// This file is the part that needs no HTTP and no process: the parsing of the arguments, the texts, and the starter
// files (`ai-desktop.cmd` and `ai-desktop` in the family folder's `bin`) that the setup writes with `cli-install`.
// What talks to the server is src/cli_run.cpp.
//
// Exit codes: 0 done, 1 failed (or, for `status`, not running), 2 wrong use.

#include <filesystem>
#include <string>
#include <vector>

namespace ai_desktop::cli {

enum class Command {
    Serve,         // no command: run the server in this process
    Start,         // start it in the background
    Stop,          // end the running one (also `quit`)
    Status,
    Open,          // open the dashboard in the browser, starting the server when needed
    Version,
    Help,
    CliInstall,    // write the starters (called by the setup)
    CliUninstall,  // take them away again
    Invalid,       // `error` says why; exit 2
};

struct Parsed {
    Command command = Command::Serve;
    bool open = false;      // start --open
    bool json = false;      // status --json
    std::string port;       // start --port N (as typed; the server checks it)
    std::string program;    // cli-install --program <exe> (empty: this executable)
    std::string error;
};

// `args` without the program name. `--port N` / `--port=N` alone is the server.
Parsed parse(const std::vector<std::string>& args);

inline constexpr int kExitOk = 0;
inline constexpr int kExitFailed = 1;
inline constexpr int kExitUsage = 2;
// A starter whose program is gone ends with this (as the starters of appkit).
inline constexpr int kExitProgramGone = 3;

std::string usage_text();
std::string version_text(const std::string& version);

// `status`: what it says for the running (`running` and the record) or the stopped server.
struct Status {
    bool running = false;
    std::string version;
    int port = 0;
    unsigned pid = 0;
    std::string instance;
    std::string config_dir;
    std::string log_file;
};
std::string status_text(const Status& status);
std::string status_json(const Status& status);  // one line, no trailing newline

// The marker every starter carries: a file of that name without it is never overwritten or removed.
inline constexpr const char* kStarterMark = "appkit starter";

// The folder of the starters: APPKIT_FAMILY_DIR\bin, else %LOCALAPPDATA%\appkit-family\bin (Windows); elsewhere
// APPKIT_FAMILY_DIR/bin, else ~/.local/bin. Empty when none of them is known.
std::filesystem::path starter_dir();

// The text of the starters. `local_app_data` is the value of %LOCALAPPDATA% (empty: none; the path is written whole).
// False and `*error` when the path cannot be named in a `.cmd` (not plain ASCII, a quote).
bool cmd_starter(const std::string& program, const std::string& local_app_data, std::string* text, std::string* error);
bool sh_starter(const std::string& program, std::string* text, std::string* error);

// Writes the starters of `command` for `program` into `dir` (made if missing); a file of that name that is not a
// starter is left alone and reported. Returns "" or the problem.
std::string install_starters(const std::filesystem::path& dir, const std::string& command,
                             const std::string& program, const std::string& local_app_data);
// Removes the starters of `command` from `dir` (only files that carry the mark), and `dir` itself when it is then
// empty. `*dir_removed` says whether it went. Returns "" or the problem.
std::string uninstall_starters(const std::filesystem::path& dir, const std::string& command, bool* dir_removed);

}  // namespace ai_desktop::cli
