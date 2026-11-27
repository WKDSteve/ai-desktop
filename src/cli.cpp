// The terminal command of ai-desktop (start, stop, status, open, --version, --help): argument parsing, texts and the
// starter files.

#include "cli.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

#include "nlohmann/json.hpp"

namespace ai_desktop::cli {

namespace fs = std::filesystem;

namespace {

constexpr const char* kCommandName = "ai-desktop";

Parsed invalid(const std::string& message) {
    Parsed parsed;
    parsed.command = Command::Invalid;
    parsed.error = message;
    return parsed;
}

std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

bool starts_with(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }

// The files of the starters of `command` in `dir`.
std::vector<fs::path> starter_files(const fs::path& dir, const std::string& command) {
#ifdef _WIN32
    return {dir / (command + ".cmd"), dir / command};
#else
    return {dir / command};
#endif
}

bool is_starter(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text.find(kStarterMark) != std::string::npos;
}

// `path` below `base` (without case: Windows paths), the rest including its first separator; empty when it is not.
std::string below(const std::string& path, std::string base) {
    while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();
    if (base.empty() || path.size() <= base.size()) return "";
    const std::string head = path.substr(0, base.size());
    const bool same = std::equal(head.begin(), head.end(), base.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    const char next = path[base.size()];
    return (same && (next == '\\' || next == '/')) ? path.substr(base.size()) : "";
}

// A verbatim prefix (\\?\C:\...) is a path cmd cannot run: never in a starter.
std::string plain_path(std::string path) {
    if (starts_with(path, "\\\\?\\UNC\\")) return "\\\\" + path.substr(8);
    if (starts_with(path, "\\\\?\\")) return path.substr(4);
    return path;
}

std::string write_starter(const fs::path& path, const std::string& text) {
    std::error_code ec;
    if (fs::exists(path, ec) && !is_starter(path)) {
        return path.string() + " is there and is not a starter: it is left as it is";
    }
    fs::path part = path;
    part += ".appkit-new";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            out.close();
            fs::remove(part, ec);
            return "cannot write " + part.string();
        }
    }
#ifndef _WIN32
    fs::permissions(part, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                              fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace, ec);
#endif
    fs::rename(part, path, ec);
    if (ec) {
        // Windows does not rename over a file that exists.
        fs::remove(path, ec);
        ec.clear();
        fs::rename(part, path, ec);
    }
    if (ec) {
        std::error_code ignored;
        fs::remove(part, ignored);
        return "cannot replace " + path.string() + ": " + ec.message();
    }
    return "";
}

}  // namespace

Parsed parse(const std::vector<std::string>& args) {
    Parsed parsed;
    if (args.empty()) return parsed;  // the server

    const std::string& first = args[0];
    // The old way: only --port. Everything else after it is the server's own check.
    if (first == "--port" || starts_with(first, "--port=")) return parsed;

    if (first == "--version" || first == "-V" || first == "version") {
        parsed.command = Command::Version;
        return args.size() == 1 ? parsed : invalid("'" + first + "' takes no argument");
    }
    if (first == "--help" || first == "-h" || first == "help") {
        parsed.command = Command::Help;
        return parsed;
    }

    if (first == "start") {
        parsed.command = Command::Start;
    } else if (first == "stop" || first == "quit") {
        parsed.command = Command::Stop;
    } else if (first == "status") {
        parsed.command = Command::Status;
    } else if (first == "open") {
        parsed.command = Command::Open;
    } else if (first == "cli-install") {
        parsed.command = Command::CliInstall;
    } else if (first == "cli-uninstall") {
        parsed.command = Command::CliUninstall;
    } else {
        return invalid(starts_with(first, "-") ? "unknown argument '" + first + "' (supported: --port N)"
                                                : "unknown command '" + first + "'");
    }

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            Parsed help;
            help.command = Command::Help;
            return help;
        }
        if (parsed.command == Command::Start && arg == "--open") {
            parsed.open = true;
        } else if (parsed.command == Command::Status && arg == "--json") {
            parsed.json = true;
        } else if ((parsed.command == Command::Start || parsed.command == Command::Open) && arg == "--port") {
            if (i + 1 >= args.size()) return invalid("--port needs a value");
            parsed.port = args[++i];
        } else if ((parsed.command == Command::Start || parsed.command == Command::Open) &&
                   starts_with(arg, "--port=")) {
            parsed.port = arg.substr(7);
        } else if (parsed.command == Command::CliInstall && arg == "--program") {
            if (i + 1 >= args.size()) return invalid("--program needs a value");
            parsed.program = args[++i];
        } else {
            return invalid("'" + first + "' does not take '" + arg + "'");
        }
    }
    return parsed;
}

std::string usage_text() {
    return "ai-desktop: local agent-framework server with a dashboard\n"
           "\n"
           "Usage:\n"
           "  ai-desktop                    run the server in this terminal (Ctrl+C ends it)\n"
           "  ai-desktop --port <n>         the same on another port (default 8080, or AI_DESKTOP_PORT)\n"
           "  ai-desktop start [--open]     start the server in the background; --open opens the dashboard\n"
           "  ai-desktop stop               end the running server (also: quit)\n"
           "  ai-desktop status [--json]    is it running? port, pid, version; exit 0 running, 1 not running\n"
           "  ai-desktop open               open the dashboard in the browser (starts the server when needed)\n"
           "  ai-desktop --version          the version\n"
           "  ai-desktop --help             this text\n"
           "\n"
           "Exit codes: 0 done, 1 failed (or, for status, not running), 2 wrong use.\n";
}

std::string version_text(const std::string& version) { return std::string(kCommandName) + " " + version + "\n"; }

std::string status_text(const Status& status) {
    if (!status.running) {
        return "ai-desktop " + status.version + ": not running\n";
    }
    std::string text = "ai-desktop " + status.version + ": running\n";
    text += "  pid:       " + std::to_string(status.pid) + "\n";
    text += "  port:      " + std::to_string(status.port) + "\n";
    text += "  dashboard: http://127.0.0.1:" + std::to_string(status.port) + "/\n";
    if (!status.config_dir.empty()) text += "  config:    " + status.config_dir + "\n";
    if (!status.log_file.empty()) text += "  log:       " + status.log_file + "\n";
    return text;
}

std::string status_json(const Status& status) {
    nlohmann::ordered_json doc;
    doc["app"] = kCommandName;
    doc["running"] = status.running;
    doc["version"] = status.version;
    if (status.running) {
        doc["pid"] = status.pid;
        doc["port"] = status.port;
        doc["url"] = "http://127.0.0.1:" + std::to_string(status.port) + "/";
        doc["instance"] = status.instance;
    }
    if (!status.config_dir.empty()) doc["config_dir"] = status.config_dir;
    if (!status.log_file.empty()) doc["log_file"] = status.log_file;
    return doc.dump();
}

fs::path starter_dir() {
    if (const std::string family = env_value("APPKIT_FAMILY_DIR"); !family.empty()) {
        return fs::path(family) / "bin";
    }
#ifdef _WIN32
    if (const std::string local = env_value("LOCALAPPDATA"); !local.empty()) {
        return fs::path(local) / "appkit-family" / "bin";
    }
    return {};
#else
    if (const std::string home = env_value("HOME"); !home.empty()) return fs::path(home) / ".local" / "bin";
    return {};
#endif
}

bool cmd_starter(const std::string& program, const std::string& local_app_data, std::string* text,
                 std::string* error) {
    std::string whole = plain_path(program);
    std::replace(whole.begin(), whole.end(), '/', '\\');
    std::string prefix;
    std::string tail = whole;
    if (const std::string rest = local_app_data.empty() ? "" : below(whole, plain_path(local_app_data));
        !rest.empty()) {
        prefix = "%LOCALAPPDATA%";
        tail = rest;
    }
    // A % of the person's path is doubled; the %LOCALAPPDATA% in front is ours.
    std::string escaped = prefix;
    for (const char c : tail) escaped += c == '%' ? std::string("%%") : std::string(1, c);
    for (const char c : escaped) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80 || u < 0x20 || c == '"') {
            if (error) {
                *error = "cmd cannot read the path " + program +
                         " in a starter (a character that is not plain ASCII, or a quote)";
            }
            return false;
        }
    }
    const std::string name = kCommandName;
    // One-line ifs: a path with ")" (Program Files (x86)) would end a block in parentheses early.
    *text = "@echo off\r\n"
            "rem " + std::string(kStarterMark) + " of " + name +
            ": starts its program with all arguments and hands its exit code back.\r\n"
            "rem Written by the install of " + name + "; it goes with its uninstall.\r\n"
            "setlocal\r\n"
            "set \"program=" + escaped + "\"\r\n"
            "if not exist \"%program%\" echo " + name +
            ": its program is gone (%program%): install it again 1>&2& exit /b " + std::to_string(kExitProgramGone) +
            "\r\n"
            "\"%program%\" %*\r\n"
            "exit /b %ERRORLEVEL%\r\n";
    return true;
}

bool sh_starter(const std::string& program, std::string* text, std::string* error) {
    std::string path = plain_path(program);
    for (const char c : path) {
        if (static_cast<unsigned char>(c) < 0x20) {
            if (error) *error = "a shell cannot be given the path " + program + " in a starter";
            return false;
        }
    }
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '\\', '/');  // Git Bash takes C:/... as it is
#endif
    std::string quoted = "'";
    for (const char c : path) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    quoted += "'";
    const std::string name = kCommandName;
    *text = "#!/bin/sh\n"
            "# " + std::string(kStarterMark) + " of " + name +
            ": starts its program with all arguments and hands its exit code back.\n"
            "# Written by the install of " + name + "; it goes with its uninstall.\n"
            "program=" + quoted + "\n"
            "if [ ! -f \"$program\" ]; then\n"
            "  echo \"" + name + ": its program is gone ($program): install it again\" >&2\n"
            "  exit " + std::to_string(kExitProgramGone) + "\n"
            "fi\n"
            "exec \"$program\" \"$@\"\n";
    return true;
}

std::string install_starters(const fs::path& dir, const std::string& command, const std::string& program,
                             const std::string& local_app_data) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return "cannot make " + dir.string() + ": " + ec.message();
    std::string text, error;
#ifdef _WIN32
    if (!cmd_starter(program, local_app_data, &text, &error)) return error;
    if (const auto problem = write_starter(dir / (command + ".cmd"), text); !problem.empty()) return problem;
#else
    (void)local_app_data;
#endif
    if (!sh_starter(program, &text, &error)) return error;
    return write_starter(dir / command, text);
}

std::string uninstall_starters(const fs::path& dir, const std::string& command, bool* dir_removed) {
    if (dir_removed) *dir_removed = false;
    std::error_code ec;
    for (const auto& file : starter_files(dir, command)) {
        if (fs::exists(file, ec) && is_starter(file)) {
            fs::remove(file, ec);
            if (ec) return "cannot remove " + file.string() + ": " + ec.message();
        }
    }
    // A write that was cut off.
    if (fs::is_directory(dir, ec)) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".appkit-new") fs::remove(entry.path(), ec);
        }
        if (fs::is_empty(dir, ec)) {
            fs::remove(dir, ec);
            if (dir_removed) *dir_removed = !ec;
        }
    }
    return "";
}

ServerConsole server_console(const char* no_console) {
    if (no_console == nullptr || *no_console == '\0') return ServerConsole::Visible;
    return std::string(no_console) == "0" ? ServerConsole::Visible : ServerConsole::None;
}

}  // namespace ai_desktop::cli
