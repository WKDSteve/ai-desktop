#include "paths.hpp"

#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>

#include <cstdint>
#include <vector>
#endif

namespace fs = std::filesystem;

namespace ai_desktop::paths {

fs::path exe_dir() {
#ifdef _WIN32
    // The longest path Windows knows is 32767 characters; the buffer grows until the name fits.
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
    return fs::path(buffer).parent_path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    std::error_code ec;
    const fs::path exe = fs::canonical(fs::path(buffer.data()), ec);
    return ec ? fs::path() : exe.parent_path();
#else
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : exe.parent_path();
#endif
}

std::string file_api_name(const fs::path& path) {
#ifdef _WIN32
    // cpp-httplib opens files by narrow name (stat, ifstream): the system's code page (ACP), not UTF-8.
    const std::wstring wide = path.wstring();
    if (wide.empty()) {
        return {};
    }
    const auto to_acp = [](const std::wstring& text) -> std::string {
        const UINT page = GetACP();
        // With CP_UTF8 neither flags nor the "default char used" answer are allowed; UTF-8 holds every name anyway.
        const DWORD flags = page == CP_UTF8 ? 0 : WC_NO_BEST_FIT_CHARS;
        BOOL lossy = FALSE;
        BOOL* lossy_ptr = page == CP_UTF8 ? nullptr : &lossy;
        const int length = WideCharToMultiByte(page, flags, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                               nullptr, lossy_ptr);
        if (length <= 0 || lossy) {
            return {};
        }
        std::string out(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(page, flags, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr,
                            nullptr);
        return out;
    };
    if (std::string name = to_acp(wide); !name.empty()) {
        return name;
    }
    // A name the code page cannot hold: its short (8.3) form is ASCII, where the volume keeps short names.
    const DWORD need = GetShortPathNameW(wide.c_str(), nullptr, 0);
    if (need == 0) {
        return {};
    }
    std::wstring short_name(need, L'\0');
    const DWORD got = GetShortPathNameW(wide.c_str(), short_name.data(), need);
    if (got == 0 || got >= need) {
        return {};
    }
    short_name.resize(got);
    return to_acp(short_name);
#else
    return path.string();
#endif
}

std::string dashboard_dir(const char* env_value, const fs::path& program_dir) {
    if (env_value && *env_value) {
        // As the environment holds it: already the narrow name the file calls take.
        return env_value;
    }
    if (program_dir.empty()) {
        return {};
    }
    return file_api_name(program_dir / "dashboard");
}

}  // namespace ai_desktop::paths
