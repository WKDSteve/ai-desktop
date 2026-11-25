// Brings the dashboard to the front when a family app hands over a new job.

#include "foreground.hpp"

#include <cstdlib>
#include <iostream>
#include <thread>

#include "legacy.hpp"
#include "session.hpp"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ai_desktop::foreground {

namespace {

#ifdef _WIN32
// The first visible top-level window whose title holds the dashboard's title (the browser adds its own name).
BOOL CALLBACK find_dashboard(HWND window, LPARAM found) {
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    wchar_t title[512];
    const int length = GetWindowTextW(window, title, 512);
    if (length <= 0) {
        return TRUE;
    }
    const std::wstring text(title, static_cast<size_t>(length));
    if (text.rfind(L"ai Dashboard", 0) == 0 || text.rfind(L"KI Dashboard", 0) == 0) {
        *reinterpret_cast<HWND*>(found) = window;
        return FALSE;
    }
    return TRUE;
}

void show(const std::string& url) {
    HWND window = nullptr;
    EnumWindows(find_dashboard, reinterpret_cast<LPARAM>(&window));
    if (window) {
        if (IsIconic(window)) {
            ShowWindow(window, SW_RESTORE);
        }
        SetForegroundWindow(window);
        return;
    }
    // The browser is another process: it may take the focus only if this one passes its right on.
    AllowSetForegroundWindow(ASFW_ANY);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const std::wstring wide(url.begin(), url.end());  // ASCII only: scheme, loopback address, port, hex id
    const auto rc =
        reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (rc <= 32) {
        std::cerr << "ai-desktop: could not open the dashboard for a job (" << rc << ")\n";
    }
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
}
#else
void show(const std::string& url) {
#ifdef __APPLE__
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
        return;  // no desktop to show it on
    }
#endif
    const pid_t child = fork();
    if (child == 0) {
        // The thread that forks has SIGINT and SIGTERM blocked (main.cpp); the browser it may start must not.
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        execlp(opener, opener, url.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child > 0) {
        int status = 0;
        waitpid(child, &status, 0);
    }
}
#endif

}  // namespace

void show_job(int port, const std::string& job_id) {
    if (const char* off = legacy::env("AI_DESKTOP_JOBS_FOREGROUND"); off && std::string(off) == "0") {
        return;
    }
    // The id is 16 hex digits (jobs::new_id), so the address holds nothing but ASCII of a fixed form. On Windows a
    // one-time ticket logs the page in (src/session.hpp): the command line of the person's browser is not readable by
    // another account. Elsewhere it is (/proc/<pid>/cmdline of xdg-open and the browser), so no ticket goes there:
    // the page asks for the console's link instead.
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/#job=" + job_id;
#ifdef _WIN32
    if (const std::string ticket = session::new_ticket(); !ticket.empty()) {
        url += "&ticket=" + ticket;
    }
#endif
    std::thread([url] { show(url); }).detach();
}

}  // namespace ai_desktop::foreground
