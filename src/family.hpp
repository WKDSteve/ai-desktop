#pragma once

// How ai-desktop tells the other apps of the family (tasks-desktop, the hub apps of appkit) that it runs: the
// discovery file `hub.json` in its config folder, in the format of appkit (`appkit_core::registry::HubRecord`,
// read with `deny_unknown_fields`, see appkit's docs/FAMILY.md). Exactly five keys -- app, port, pid, version,
// instance -- pretty-printed with two spaces and a final newline, as serde_json writes it. No secret: any local
// program may read it. Written once the port is bound, through a temporary file and a rename, removed when the
// server ends; a file left by a killed process does not count, since the reader checks that `pid` is alive.

#include <cstdint>
#include <filesystem>
#include <string>

namespace ai_desktop::family {

// The name the app gives itself in `hub.json` and in the family folder.
inline constexpr const char* kApp = "ai-desktop";

struct HubRecord {
    std::string app = kApp;
    int port = 0;
    std::uint32_t pid = 0;
    std::string version;
    // 32 hex digits, new with every start.
    std::string instance;
};

// The folder of `hub.json` by appkit's rule (`appkit_core::family::config_dir`): AI_DESKTOP_CONFIG_DIR, else
// %APPDATA%\ai-desktop (Windows), ~/Library/Application Support/ai-desktop (macOS), $XDG_CONFIG_HOME/ai-desktop
// or ~/.config/ai-desktop. Unlike config::directory() never the old loomai folder: the other apps do not look
// there. Empty when none of the variables is set.
std::filesystem::path config_dir();

// The text of `hub.json`, byte for byte what appkit writes for the same record.
std::string record_text(const HubRecord& record);

// `<dir>/hub.json`.
std::filesystem::path record_path(const std::filesystem::path& dir);

// Writes `hub.json` into `dir` (made if missing) through `hub.json.tmp` and a rename. False with `*error` set
// when it cannot.
bool write_record(const std::filesystem::path& dir, const HubRecord& record, std::string* error);

// Reads `hub.json` of `dir`; false when it is not there or not a record of this app.
bool read_record(const std::filesystem::path& dir, HubRecord* record);

// Removes `hub.json` from `dir` -- but only when it is still the file of `instance`, so an ending server never
// takes away the file of another one that started meanwhile.
void remove_record(const std::filesystem::path& dir, const std::string& instance);

// A new instance id: 32 lowercase hex digits.
std::string new_instance();

// The id of this process.
std::uint32_t current_pid();

}  // namespace ai_desktop::family
