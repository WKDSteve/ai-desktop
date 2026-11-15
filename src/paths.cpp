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

fs::path dashboard_dir(const char* env_value, const fs::path& program_dir) {
    if (env_value && *env_value) {
        return fs::path(env_value);
    }
    if (program_dir.empty()) {
        return {};
    }
    return program_dir / "dashboard";
}

}  // namespace ai_desktop::paths
