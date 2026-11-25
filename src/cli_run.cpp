// Runs the terminal commands of ai-desktop (start, stop, status, open) against the server and returns their exit
// codes.

#include "cli_run.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

#include "httplib.h"
#include "nlohmann/json.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "control_secret.hpp"
#include "family.hpp"
#include "process.hpp"

namespace ai_desktop::cli {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

fs::path log_path(const fs::path& config_dir) { return config_dir / "server.log"; }

// Where to look when a start fails: on Windows the server has a console window of its own, elsewhere a log file.
std::string where_to_look(const fs::path& config_dir) {
#ifdef _WIN32
    (void)config_dir;
    return "";
#else
    return " (see " + log_path(config_dir).string() + ")";
#endif
}

// Whether /health on `port` answers as ai-desktop (and, when `instance` is given, as that instance); `*version` is
// what it says.
bool answers(int port, const std::string& instance, std::string* version) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(3, 0);
    const auto res = client.Get("/health");
    if (!res || res->status != 200) return false;
    const json body = json::parse(res->body, nullptr, false);
    if (!body.is_object() || body.value("app", "") != family::kApp) return false;
    if (!instance.empty() && body.value("instance", "") != instance) return false;
    if (version) *version = body.value("version", "");
    return true;
}

// Whether the process `pid` is gone. The end of a stop is the end of the process: a server that is ending stops
// answering before it stops listening, and a refused connection is slow and ambiguous on Windows.
bool process_gone(std::uint32_t pid) {
#ifdef _WIN32
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!handle) return true;
    const bool gone = WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
    CloseHandle(handle);
    return gone;
#else
    return kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
#endif
}

// The running server of this config folder: hub.json names it and /health on its port answers as that instance.
bool find_running(const fs::path& config_dir, family::HubRecord* record) {
    family::HubRecord found;
    if (config_dir.empty() || !family::read_record(config_dir, &found)) return false;
    std::string live;
    if (!answers(found.port, found.instance, &live)) return false;
    if (!live.empty()) found.version = live;
    *record = found;
    return true;
}

struct Spawned {
    std::uint32_t pid = 0;
#ifdef _WIN32
    HANDLE handle = nullptr;
#endif
};

#ifdef _WIN32
std::wstring widen(const std::string& text) {
    if (text.empty()) return L"";
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}
#endif

// The server in its own place. Windows: a console window of its own, visible, with nothing inherited from this
// process -- the same as the tile of hub-desktop (docs/PROCESSES.md): a process that stays is never hidden
// (no CREATE_NO_WINDOW, no DETACHED_PROCESS), closing the window ends the server, `ai-desktop stop` does too.
// Elsewhere it is a session of its own with its output in `log`.
bool spawn_server(const std::string& exe, const std::vector<std::string>& args, const fs::path& log,
                  Spawned* spawned, std::string* error) {
#ifdef _WIN32
    (void)log;
    std::wstring command = widen(process::quote_windows_arg(exe));
    for (const auto& arg : args) command += L" " + widen(process::quote_windows_arg(arg));

    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION info{};
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                   CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &info);
    if (!ok) {
        *error = "cannot start " + exe + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(info.hThread);
    spawned->pid = info.dwProcessId;
    spawned->handle = info.hProcess;
    return true;
#else
    std::error_code ec;
    fs::create_directories(log.parent_path(), ec);
    const pid_t pid = fork();
    if (pid < 0) {
        *error = "cannot fork";
        return false;
    }
    if (pid == 0) {
        setsid();
        const int out = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        const int in = open("/dev/null", O_RDONLY);
        if (out >= 0) {
            dup2(out, 1);
            dup2(out, 2);
        }
        if (in >= 0) dup2(in, 0);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execv(exe.c_str(), argv.data());
        _exit(127);
    }
    spawned->pid = static_cast<std::uint32_t>(pid);
    return true;
#endif
}

bool has_exited(const Spawned& spawned) {
#ifdef _WIN32
    return spawned.handle && WaitForSingleObject(spawned.handle, 0) == WAIT_OBJECT_0;
#else
    int status = 0;
    return waitpid(static_cast<pid_t>(spawned.pid), &status, WNOHANG) != 0;
#endif
}

void release(Spawned& spawned) {
#ifdef _WIN32
    if (spawned.handle) CloseHandle(spawned.handle);
    spawned.handle = nullptr;
#else
    (void)spawned;
#endif
}

Status to_status(const family::HubRecord& record, const fs::path& config_dir) {
    Status status;
    status.running = true;
    status.version = record.version;
    status.port = record.port;
    status.pid = record.pid;
    status.instance = record.instance;
    status.config_dir = config_dir.string();
#ifndef _WIN32
    status.log_file = log_path(config_dir).string();
#endif
    return status;
}

// Starts the server unless it runs. 0 started or was running; 1 failed. `*record` is the running one.
int ensure_running(const Parsed& parsed, const std::string& self_exe, const fs::path& config_dir,
                   family::HubRecord* record, bool* started) {
    *started = false;
    if (find_running(config_dir, record)) return kExitOk;

    std::vector<std::string> args;
    if (!parsed.port.empty()) {
        args = {"--port", parsed.port};
    }
    std::string error;
    Spawned spawned;
    if (!spawn_server(self_exe, args, log_path(config_dir), &spawned, &error)) {
        std::cerr << "ai-desktop: " << error << "\n";
        return kExitFailed;
    }
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (find_running(config_dir, record) && record->pid == spawned.pid) {
            release(spawned);
            *started = true;
            return kExitOk;
        }
        if (has_exited(spawned)) {
            release(spawned);
            std::cerr << "ai-desktop: the server ended right after it started (port taken?)"
                      << where_to_look(config_dir) << "\n";
            return kExitFailed;
        }
        std::this_thread::sleep_for(100ms);
    }
    release(spawned);
    std::cerr << "ai-desktop: the server did not answer within 20 seconds" << where_to_look(config_dir) << "\n";
    return kExitFailed;
}

void open_in_browser(const std::string& url) {
    const char* skip = std::getenv("AI_DESKTOP_NO_BROWSER");
    if (skip && *skip && std::string(skip) != "0") return;
#ifdef _WIN32
    const std::wstring wide = widen(url);
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
#ifdef __APPLE__
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    const pid_t pid = fork();
    if (pid == 0) {
        execlp(opener, opener, url.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
#endif
}

// The dashboard in the browser. On Windows the link carries a one-time ticket (the console's link does too), so the
// dashboard is logged in; elsewhere no ticket goes into a command line another user can read.
int open_dashboard(const family::HubRecord& record, const fs::path& config_dir) {
    const std::string base = "http://127.0.0.1:" + std::to_string(record.port) + "/";
    std::string url = base;
#ifdef _WIN32
    if (const std::string secret = control_secret::read(config_dir); !secret.empty()) {
        httplib::Client client("127.0.0.1", record.port);
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(5, 0);
        const auto res = client.Post("/session/ticket", {{"Authorization", "Bearer " + secret}}, "", "application/json");
        if (res && res->status == 200) {
            const json body = json::parse(res->body, nullptr, false);
            if (body.is_object() && body.contains("ticket") && body["ticket"].is_string()) {
                url += "#ticket=" + body["ticket"].get<std::string>();
            }
        }
    }
#else
    (void)config_dir;
#endif
    open_in_browser(url);
    std::cout << "Dashboard: " << base << "\n";
    return kExitOk;
}

int do_start(const Parsed& parsed, const std::string& self_exe, const std::string& version) {
    const fs::path config_dir = family::config_dir();
    if (config_dir.empty()) {
        std::cerr << "ai-desktop: no config folder (set AI_DESKTOP_CONFIG_DIR)\n";
        return kExitFailed;
    }
    family::HubRecord record;
    bool started = false;
    if (const int rc = ensure_running(parsed, self_exe, config_dir, &record, &started); rc != kExitOk) return rc;
    if (started) {
        std::cout << "ai-desktop " << version << " started: pid " << record.pid << ", http://127.0.0.1:" << record.port
                  << "/\n";
    } else {
        std::cout << "ai-desktop is already running: pid " << record.pid << ", http://127.0.0.1:" << record.port
                  << "/\n";
    }
    return parsed.open ? open_dashboard(record, config_dir) : kExitOk;
}

int do_open(const Parsed& parsed, const std::string& self_exe) {
    const fs::path config_dir = family::config_dir();
    if (config_dir.empty()) {
        std::cerr << "ai-desktop: no config folder (set AI_DESKTOP_CONFIG_DIR)\n";
        return kExitFailed;
    }
    family::HubRecord record;
    bool started = false;
    if (const int rc = ensure_running(parsed, self_exe, config_dir, &record, &started); rc != kExitOk) return rc;
    return open_dashboard(record, config_dir);
}

int do_stop() {
    const fs::path config_dir = family::config_dir();
    family::HubRecord record;
    if (!find_running(config_dir, &record)) {
        std::cout << "ai-desktop is not running\n";
        return kExitOk;
    }
    const std::string secret = control_secret::read(config_dir);
    if (secret.empty()) {
        std::cerr << "ai-desktop: cannot stop pid " << record.pid << ": no control secret in "
                  << control_secret::path_in(config_dir).string() << "\n";
        return kExitFailed;
    }
    httplib::Client client("127.0.0.1", record.port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(10, 0);
    const auto res = client.Post("/shutdown", {{"Authorization", "Bearer " + secret}}, "", "application/json");
    if (!res || res->status != 200) {
        std::cerr << "ai-desktop: the server did not accept the stop (pid " << record.pid << ")\n";
        return kExitFailed;
    }
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (process_gone(record.pid)) {
            std::cout << "ai-desktop stopped\n";
            return kExitOk;
        }
        std::this_thread::sleep_for(100ms);
    }
    std::cerr << "ai-desktop: the server still runs 15 seconds after the stop (pid " << record.pid << ")\n";
    return kExitFailed;
}

int do_status(const Parsed& parsed, const std::string& version) {
    const fs::path config_dir = family::config_dir();
    family::HubRecord record;
    Status status;
    const bool running = find_running(config_dir, &record);
    if (running) {
        status = to_status(record, config_dir);
    } else {
        status.version = version;
        status.config_dir = config_dir.string();
#ifndef _WIN32
        status.log_file = log_path(config_dir).string();
#endif
    }
    if (parsed.json) {
        std::cout << status_json(status) << "\n";
    } else {
        std::cout << status_text(status);
    }
    return running ? kExitOk : kExitFailed;
}

// Called by the setup: the starters of `ai-desktop` for this program. The PATH is the setup's business.
int do_cli_install(const Parsed& parsed, const std::string& self_exe) {
    const fs::path dir = starter_dir();
    if (dir.empty()) {
        std::cerr << "ai-desktop: no folder for the starters (set APPKIT_FAMILY_DIR)\n";
        return kExitFailed;
    }
    const std::string program = parsed.program.empty() ? self_exe : parsed.program;
    const char* local = std::getenv("LOCALAPPDATA");
    if (const std::string problem = install_starters(dir, "ai-desktop", program, local ? local : "");
        !problem.empty()) {
        std::cerr << "ai-desktop: " << problem << "\n";
        return kExitFailed;
    }
    std::cout << "starters in " << dir.string() << "\n";
    return kExitOk;
}

int do_cli_uninstall() {
    const fs::path dir = starter_dir();
    if (dir.empty()) return kExitOk;
    bool removed = false;
    if (const std::string problem = uninstall_starters(dir, "ai-desktop", &removed); !problem.empty()) {
        std::cerr << "ai-desktop: " << problem << "\n";
        return kExitFailed;
    }
    // The setup reads this to know that the folder is empty and its PATH entry may go.
    std::cout << (removed ? "starter folder removed: " : "starters removed from ") << dir.string() << "\n";
    return kExitOk;
}

}  // namespace

std::string own_executable(const char* argv0) {
#ifdef _WIN32
    (void)argv0;
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return fs::path(buffer).string();
#else
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return self.string();
    return fs::absolute(argv0, ec).string();
#endif
}

int run(const Parsed& parsed, const std::string& self_exe, const std::string& version) {
    switch (parsed.command) {
        case Command::Version:
            std::cout << version_text(version);
            return kExitOk;
        case Command::Help:
            std::cout << usage_text();
            return kExitOk;
        case Command::Start:
            return do_start(parsed, self_exe, version);
        case Command::Open:
            return do_open(parsed, self_exe);
        case Command::Stop:
            return do_stop();
        case Command::Status:
            return do_status(parsed, version);
        case Command::CliInstall:
            return do_cli_install(parsed, self_exe);
        case Command::CliUninstall:
            return do_cli_uninstall();
        case Command::Serve:
        case Command::Invalid:
            break;
    }
    return kExitUsage;
}

}  // namespace ai_desktop::cli
