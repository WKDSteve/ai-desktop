#include "process.hpp"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace ai_desktop::process {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kStderrTail = 8 * 1024;

void keep_tail(std::string& buffer, const char* data, std::size_t size) {
    buffer.append(data, size);
    if (buffer.size() > kStderrTail) {
        buffer.erase(0, buffer.size() - kStderrTail);
    }
}

// Splits what was read into lines; `pending` holds an unfinished line.
void feed_lines(std::string& pending, const char* data, std::size_t size, std::size_t max_line,
                const std::function<void(const std::string&)>& on_line) {
    for (std::size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (c == '\n') {
            if (!pending.empty() && pending.back() == '\r') {
                pending.pop_back();
            }
            on_line(pending);
            pending.clear();
        } else if (pending.size() < max_line) {
            pending.push_back(c);
        }
    }
}

}  // namespace

std::string quote_windows_arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
        } else {
            out.append(backslashes, '\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

std::vector<std::string> env_names() {
    std::vector<std::string> names;
#ifdef _WIN32
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            const std::wstring entry = p;
            const std::size_t eq = entry.find(L'=', 1);
            if (eq == std::wstring::npos) continue;
            const std::wstring name = entry.substr(0, eq);
            const int size = WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0,
                                                 nullptr, nullptr);
            std::string narrow(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), narrow.data(), size, nullptr,
                                nullptr);
            names.push_back(narrow);
        }
        FreeEnvironmentStringsW(block);
    }
#else
    for (char** e = environ; e && *e; ++e) {
        const std::string entry = *e;
        names.push_back(entry.substr(0, entry.find('=')));
    }
#endif
    return names;
}

fs::path find_on_path(const std::vector<std::string>& names) {
    const char* path = std::getenv("PATH");
    if (!path) {
        return {};
    }
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    const std::string text = path;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(separator, start), text.size());
        std::string dir = text.substr(start, end - start);
        start = end + 1;
        if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') {
            dir = dir.substr(1, dir.size() - 2);
        }
        if (dir.empty()) {
            continue;
        }
        for (const auto& name : names) {
            std::error_code ec;
            const fs::path candidate = fs::u8path(dir) / fs::u8path(name);
            if (fs::is_regular_file(candidate, ec)) {
                return candidate;
            }
        }
    }
    return {};
}

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::wstring upper(std::wstring text) {
    for (auto& c : text) {
        c = static_cast<wchar_t>(towupper(c));
    }
    return text;
}

// The environment of this process with `changes` applied, as the sorted block CreateProcessW takes.
std::wstring environment_block(const std::vector<std::pair<std::string, std::string>>& changes) {
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> vars;  // UPPER name -> (name, value)
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            const std::wstring entry = p;
            const std::size_t eq = entry.find(L'=', 1);  // "=C:=C:\..." entries start with '='
            if (eq == std::wstring::npos) {
                continue;
            }
            const std::wstring name = entry.substr(0, eq);
            vars[upper(name)] = {name, entry.substr(eq + 1)};
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [name, value] : changes) {
        const std::wstring wide = widen(name);
        if (value.empty()) {
            vars.erase(upper(wide));
        } else {
            vars[upper(wide)] = {wide, widen(value)};
        }
    }
    std::wstring out;
    for (const auto& item : vars) {
        out += item.second.first + L"=" + item.second.second;
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

class WinChild final : public Child {
public:
    WinChild(HANDLE process, HANDLE job, HANDLE out_read, HANDLE err_read, HANDLE in_write, std::string input)
        : process_(process), job_(job), out_read_(out_read), err_read_(err_read) {
        in_thread_ = std::thread([in_write, input = std::move(input)] {
            const char* data = input.data();
            std::size_t left = input.size();
            while (left > 0) {
                DWORD written = 0;
                const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(left, 64 * 1024));
                if (!WriteFile(in_write, data, chunk, &written, nullptr) || written == 0) {
                    break;  // the child closed stdin or ended
                }
                data += written;
                left -= written;
            }
            CloseHandle(in_write);
        });
        err_thread_ = std::thread([this] {
            char buffer[4096];
            DWORD got = 0;
            while (ReadFile(err_read_, buffer, sizeof buffer, &got, nullptr) && got > 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                keep_tail(err_, buffer, got);
            }
        });
        // The run ends with the child, not with its output: a descendant that inherited stdout or stderr (a server
        // Claude Code left behind, a hook's background job) would keep the pipes open, and read_lines() would wait
        // for it. When the child ends, the job takes the rest of the tree, and the pipes reach their end; what the
        // child wrote before stays in the pipe and is still read.
        watch_thread_ = std::thread([this] {
            WaitForSingleObject(process_, INFINITE);
            TerminateJobObject(job_, 1);
        });
    }

    ~WinChild() override {
        kill();
        join();
        CloseHandle(out_read_);
        CloseHandle(err_read_);
        CloseHandle(process_);
        CloseHandle(job_);
    }

    void read_lines(const std::function<void(const std::string&)>& on_line, std::size_t max_line) override {
        std::string pending;
        char buffer[8192];
        DWORD got = 0;
        while (ReadFile(out_read_, buffer, sizeof buffer, &got, nullptr) && got > 0) {
            feed_lines(pending, buffer, got, max_line, on_line);
        }
        if (!pending.empty()) {
            on_line(pending);
        }
    }

    int wait() override {
        WaitForSingleObject(process_, INFINITE);
        DWORD code = 0;
        const bool known = GetExitCodeProcess(process_, &code) != 0;
        // The children of the child may still hold its stderr open; they belong to the job and go with it (the
        // watcher did that already).
        TerminateJobObject(job_, 1);
        join();
        return (killed_ || !known) ? -1 : static_cast<int>(code);
    }

    void kill() override {
        killed_ = true;
        TerminateJobObject(job_, 1);
    }

    std::string stderr_tail() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return err_;
    }

private:
    void join() {
        if (in_thread_.joinable()) in_thread_.join();
        if (err_thread_.joinable()) err_thread_.join();
        if (watch_thread_.joinable()) watch_thread_.join();
    }

    HANDLE process_;
    HANDLE job_;
    HANDLE out_read_;
    HANDLE err_read_;
    std::thread in_thread_;
    std::thread err_thread_;
    std::thread watch_thread_;
    std::mutex mutex_;
    std::string err_;
    std::atomic<bool> killed_{false};
};

std::string last_error(const char* what) {
    return std::string(what) + " failed (" + std::to_string(GetLastError()) + ")";
}

}  // namespace

std::unique_ptr<Child> start(const Spec& spec, std::string* error) {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    const auto close_all = [&] {
        for (HANDLE h : {in_r, in_w, out_r, out_w, err_r, err_w}) {
            if (h) CloseHandle(h);
        }
    };
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0)) {
        if (error) *error = last_error("CreatePipe");
        close_all();
        return nullptr;
    }
    // The parent's ends are not inherited.
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    // Only the three pipe ends go to the child: an inheritable socket of the server must not.
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buffer(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buffer.data());
    HANDLE inherit[3] = {in_r, out_w, err_w};
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr,
                                   nullptr)) {
        if (error) *error = last_error("UpdateProcThreadAttribute");
        close_all();
        return nullptr;
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_r;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = err_w;
    si.lpAttributeList = attrs;

    std::string command = quote_windows_arg(spec.program.u8string());
    for (const auto& arg : spec.args) {
        command += " " + quote_windows_arg(arg);
    }
    std::wstring wide_command = widen(command);
    std::wstring env = environment_block(spec.env);
    const std::wstring program = spec.program.wstring();
    const std::wstring cwd = spec.cwd.wstring();

    PROCESS_INFORMATION pi{};
    const BOOL created =
        CreateProcessW(program.c_str(), wide_command.data(), nullptr, nullptr, TRUE,
                       EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,
                       env.data(), cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    if (!created) {
        if (error) *error = last_error("CreateProcess") + " for " + spec.program.u8string();
        close_all();
        return nullptr;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
        !AssignProcessToJobObject(job, pi.hProcess)) {
        if (error) *error = last_error("the job object");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (job) CloseHandle(job);
        close_all();
        return nullptr;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    // The child's ends belong to the child now.
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(err_w);
    return std::make_unique<WinChild>(pi.hProcess, job, out_r, err_r, in_w, spec.stdin_text);
}

#else  // POSIX

namespace {

class PosixChild final : public Child {
public:
    PosixChild(pid_t pid, int out_fd, int err_fd, int in_fd, std::string input)
        : pid_(pid), out_fd_(out_fd), err_fd_(err_fd) {
        in_thread_ = std::thread([in_fd, input = std::move(input)] {
            const char* data = input.data();
            std::size_t left = input.size();
            while (left > 0) {
                const ssize_t written = ::write(in_fd, data, left);
                if (written <= 0) break;
                data += written;
                left -= static_cast<std::size_t>(written);
            }
            ::close(in_fd);
        });
        err_thread_ = std::thread([this] {
            char buffer[4096];
            ssize_t got = 0;
            while ((got = ::read(err_fd_, buffer, sizeof buffer)) > 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                keep_tail(err_, buffer, static_cast<std::size_t>(got));
            }
        });
        // The run ends with the child, not with its output (see WinChild): when the child ends, its process group
        // goes, so a descendant that inherited stdout or stderr cannot keep read_lines() waiting. waitid(WNOWAIT)
        // leaves the child a zombie until the group is killed: its pid (the group's id) cannot be taken by another
        // process before that kill.
        watch_thread_ = std::thread([this] {
            siginfo_t info{};
            while (::waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOWAIT) < 0 && errno == EINTR) {
            }
            std::lock_guard<std::mutex> lock(reap_mutex_);
            ::kill(-pid_, SIGKILL);  // what it started and left behind goes with it
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
            status_ = status;
            reaped_ = true;
        });
    }

    ~PosixChild() override {
        kill();
        if (watch_thread_.joinable()) watch_thread_.join();
        if (in_thread_.joinable()) in_thread_.join();
        if (err_thread_.joinable()) err_thread_.join();
        ::close(out_fd_);
        ::close(err_fd_);
    }

    void read_lines(const std::function<void(const std::string&)>& on_line, std::size_t max_line) override {
        std::string pending;
        char buffer[8192];
        ssize_t got = 0;
        while ((got = ::read(out_fd_, buffer, sizeof buffer)) > 0) {
            feed_lines(pending, buffer, static_cast<std::size_t>(got), max_line, on_line);
        }
        if (!pending.empty()) {
            on_line(pending);
        }
    }

    int wait() override {
        if (watch_thread_.joinable()) watch_thread_.join();
        const int status = status_;
        if (in_thread_.joinable()) in_thread_.join();
        if (err_thread_.joinable()) err_thread_.join();
        if (killed_ || !WIFEXITED(status)) return -1;
        return WEXITSTATUS(status);
    }

    void kill() override {
        killed_ = true;
        std::lock_guard<std::mutex> lock(reap_mutex_);
        if (!reaped_) ::kill(-pid_, SIGKILL);
    }

    std::string stderr_tail() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return err_;
    }

private:
    pid_t pid_;
    int out_fd_;
    int err_fd_;
    std::thread in_thread_;
    std::thread err_thread_;
    std::thread watch_thread_;
    std::mutex mutex_;
    std::string err_;
    std::mutex reap_mutex_;
    int status_ = 0;  // written by the watcher before it ends; read after joining it
    std::atomic<bool> killed_{false};
    std::atomic<bool> reaped_{false};
};

}  // namespace

std::unique_ptr<Child> start(const Spec& spec, std::string* error) {
    int in_pipe[2], out_pipe[2], err_pipe[2];
    if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
        if (error) *error = "pipe failed";
        return nullptr;
    }
    // Everything the child needs is built before fork(): after it only async-signal-safe calls.
    std::vector<std::string> arg_text{spec.program.string()};
    arg_text.insert(arg_text.end(), spec.args.begin(), spec.args.end());
    std::vector<char*> argv;
    for (auto& a : arg_text) argv.push_back(a.data());
    argv.push_back(nullptr);

    std::map<std::string, std::string> vars;
    for (char** e = environ; e && *e; ++e) {
        const std::string entry = *e;
        const std::size_t eq = entry.find('=');
        if (eq != std::string::npos) vars[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    for (const auto& [name, value] : spec.env) {
        if (value.empty()) vars.erase(name);
        else vars[name] = value;
    }
    std::vector<std::string> env_text;
    for (const auto& [name, value] : vars) env_text.push_back(name + "=" + value);
    std::vector<char*> envp;
    for (auto& e : env_text) envp.push_back(e.data());
    envp.push_back(nullptr);
    const std::string cwd = spec.cwd.string();
    const long max_fd = std::min(::sysconf(_SC_OPEN_MAX), 4096L);

    const pid_t pid = ::fork();
    if (pid < 0) {
        if (error) *error = "fork failed";
        return nullptr;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        ::dup2(in_pipe[0], 0);
        ::dup2(out_pipe[1], 1);
        ::dup2(err_pipe[1], 2);
        for (long fd = 3; fd < max_fd; ++fd) ::close(static_cast<int>(fd));
        // The server's threads block SIGINT and SIGTERM (main.cpp waits for them in a thread of its own), and the
        // mask survives execve: the child starts with none blocked, so a SIGTERM to it or its tree works.
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(126);
        ::execve(argv[0], argv.data(), envp.data());
        ::_exit(127);
    }
    ::setpgid(pid, pid);
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    for (int fd : {in_pipe[1], out_pipe[0], err_pipe[0]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return std::make_unique<PosixChild>(pid, out_pipe[0], err_pipe[0], in_pipe[1], spec.stdin_text);
}

#endif

int run_capture(const Spec& spec, std::chrono::milliseconds timeout, std::size_t max_out, std::string& out,
                std::string* error) {
    out.clear();
    auto child = start(spec, error);
    if (!child) {
        return -3;
    }
    std::mutex mutex;
    std::condition_variable done_cv;
    bool done = false;
    std::atomic<bool> timed_out{false};
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(mutex);
        if (!done_cv.wait_for(lock, timeout, [&] { return done; })) {
            timed_out = true;
            child->kill();
        }
    });
    child->read_lines(
        [&](const std::string& line) {
            if (out.size() < max_out) {
                out.append(line, 0, std::min(line.size(), max_out - out.size()));
                out.push_back('\n');
            }
        },
        max_out);
    const int code = child->wait();
    {
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
    }
    done_cv.notify_all();
    watchdog.join();
    if (timed_out) {
        if (error) *error = "it did not end within " + std::to_string(timeout.count() / 1000) + " s";
        return -2;
    }
    if (code != 0 && error) {
        *error = child->stderr_tail();
    }
    return code;
}

}  // namespace ai_desktop::process
