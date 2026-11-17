#include "family.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <system_error>

#include "nlohmann/json.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace ai_desktop::family {

namespace {

#ifdef _WIN32
fs::path env_path(const wchar_t* name) {
    const wchar_t* value = _wgetenv(name);
    return (value && *value) ? fs::path(value) : fs::path();
}
#else
fs::path env_path(const char* name) {
    const char* value = std::getenv(name);
    return (value && *value) ? fs::path(value) : fs::path();
}
#endif

}  // namespace

fs::path config_dir() {
#ifdef _WIN32
    if (auto dir = env_path(L"AI_DESKTOP_CONFIG_DIR"); !dir.empty()) return dir;
    if (auto base = env_path(L"APPDATA"); !base.empty()) return base / kApp;
#else
    if (auto dir = env_path("AI_DESKTOP_CONFIG_DIR"); !dir.empty()) return dir;
#ifdef __APPLE__
    if (auto home = env_path("HOME"); !home.empty()) return home / "Library" / "Application Support" / kApp;
#else
    if (auto base = env_path("XDG_CONFIG_HOME"); !base.empty()) return base / kApp;
    if (auto home = env_path("HOME"); !home.empty()) return home / ".config" / kApp;
#endif
#endif
    return {};
}

std::string record_text(const HubRecord& record) {
    // ordered_json keeps the order of the struct, which is the order of the fields of appkit's HubRecord.
    nlohmann::ordered_json doc;
    doc["app"] = record.app;
    doc["port"] = record.port;
    doc["pid"] = record.pid;
    doc["version"] = record.version;
    doc["instance"] = record.instance;
    return doc.dump(2) + "\n";
}

fs::path record_path(const fs::path& dir) {
    return dir / "hub.json";
}

bool write_record(const fs::path& dir, const HubRecord& record, std::string* error) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        if (error) *error = "cannot create " + dir.u8string() + ": " + ec.message();
        return false;
    }
    const fs::path tmp = dir / "hub.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << record_text(record);
        out.flush();
        if (!out) {
            if (error) *error = "cannot write " + tmp.u8string();
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, record_path(dir), ec);
    if (ec) {
        if (error) *error = "cannot replace " + record_path(dir).u8string() + ": " + ec.message();
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return false;
    }
    return true;
}

void remove_record(const fs::path& dir, const std::string& instance) {
    const fs::path path = record_path(dir);
    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const auto doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("instance") || doc["instance"] != instance) {
        return;
    }
    std::error_code ec;
    fs::remove(path, ec);
}

std::string new_instance() {
    // Not a secret, only different on every start: the random device, mixed with the time and the process id
    // (an old MinGW random_device was deterministic).
    std::random_device device;
    std::seed_seq seed{device(), device(), device(), device(),
                       static_cast<unsigned>(std::chrono::high_resolution_clock::now().time_since_epoch().count()),
                       static_cast<unsigned>(current_pid())};
    std::mt19937_64 gen(seed);
    char buffer[33];
    std::snprintf(buffer, sizeof buffer, "%016llx%016llx", static_cast<unsigned long long>(gen()),
                  static_cast<unsigned long long>(gen()));
    return std::string(buffer, 32);
}

std::uint32_t current_pid() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

}  // namespace ai_desktop::family
