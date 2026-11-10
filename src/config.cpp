#include "config.hpp"

#include "legacy.hpp"

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace ai_desktop::config {

namespace {

constexpr int kVersion = 1;
constexpr size_t kMaxModelNameLength = 200;

struct SettingDef {
    const char* key;
    const char* env;       // environment variable that locks the setting, or nullptr
    const char* fallback;  // built-in default
};

constexpr SettingDef kSettings[] = {
    {"theme", nullptr, "system"},
    {"default_model", "AI_DESKTOP_DEFAULT_MODEL", "llama3.1:8b-q5_K_M"},
    {"ollama_host", "AI_DESKTOP_OLLAMA_HOST", "http://127.0.0.1:11434"},
    // Context window (tokens) every Ollama request asks for. Without one Ollama
    // picks a default that depends on the GPU -- 4096 on a 12 GB card -- and
    // silently drops the beginning of a longer prompt. "0" sends none.
    {"ollama_num_ctx", "AI_DESKTOP_OLLAMA_NUM_CTX", "32768"},
    {"language", nullptr, "de"},
};

// Largest context window accepted; current models top out at about a million tokens.
constexpr long kMaxNumCtx = 1048576;

// A whole number of tokens from 0 to kMaxNumCtx, digits only (no sign, no blanks).
bool parse_num_ctx(const std::string& text, int& out) {
    if (text.empty() || text.size() > 7) {
        return false;
    }
    long value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    if (value > kMaxNumCtx) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

// Prices in USD per million tokens, as published by the providers when this
// table was written (2026-10); they change, so the stored table may override any
// entry. Ollama models are local and need no row: the usage module prices them
// at zero. A key is matched exactly first, then as the longest prefix of the
// model name (so "claude-haiku-4-5" also covers a dated snapshot of it).
const json& builtin_prices() {
    static const json table = {
        {"claude-haiku-5-5", {{"input_per_mtok", 0.10}, {"output_per_mtok", 0.50}}},
        {"claude-sonnet-5-5", {{"input_per_mtok", 2.0}, {"output_per_mtok", 10.0}}},
        {"claude-opus-5-5", {{"input_per_mtok", 4.0}, {"output_per_mtok", 20.0}}},
        {"claude-sonnet-5", {{"input_per_mtok", 2.0}, {"output_per_mtok", 10.0}}},
        {"claude-opus-5", {{"input_per_mtok", 5.0}, {"output_per_mtok", 25.0}}},
        {"claude-opus-4-8", {{"input_per_mtok", 5.0}, {"output_per_mtok", 25.0}}},
        {"claude-opus-4-7", {{"input_per_mtok", 5.0}, {"output_per_mtok", 25.0}}},
        {"claude-opus-4-6", {{"input_per_mtok", 5.0}, {"output_per_mtok", 25.0}}},
        {"claude-sonnet-4-6", {{"input_per_mtok", 3.0}, {"output_per_mtok", 15.0}}},
        {"claude-haiku-4-5", {{"input_per_mtok", 1.0}, {"output_per_mtok", 5.0}}},
        {"claude-3-5-haiku", {{"input_per_mtok", 0.80}, {"output_per_mtok", 4.0}}},
        {"claude-3-5-sonnet", {{"input_per_mtok", 3.0}, {"output_per_mtok", 15.0}}},
        {"gpt-4o", {{"input_per_mtok", 2.50}, {"output_per_mtok", 10.0}}},
        {"gpt-4o-mini", {{"input_per_mtok", 0.15}, {"output_per_mtok", 0.60}}},
        {"gpt-4.1", {{"input_per_mtok", 2.0}, {"output_per_mtok", 8.0}}},
        {"gpt-4.1-mini", {{"input_per_mtok", 0.40}, {"output_per_mtok", 1.60}}},
        {"o1", {{"input_per_mtok", 15.0}, {"output_per_mtok", 60.0}}},
        {"o3", {{"input_per_mtok", 2.0}, {"output_per_mtok", 8.0}}},
        {"o3-mini", {{"input_per_mtok", 1.10}, {"output_per_mtok", 4.40}}},
        {"gemini-2.5-pro", {{"input_per_mtok", 1.25}, {"output_per_mtok", 10.0}}},
        {"gemini-2.5-flash", {{"input_per_mtok", 0.30}, {"output_per_mtok", 2.50}}},
        {"gemini-2.0-flash", {{"input_per_mtok", 0.10}, {"output_per_mtok", 0.40}}},
    };
    return table;
}

std::mutex g_mutex;
bool g_loaded = false;
json g_doc;

const SettingDef* find_setting(const std::string& key) {
    for (const auto& def : kSettings) {
        if (key == def.key) {
            return &def;
        }
    }
    return nullptr;
}

// Non-empty value of an environment variable, or nullptr. An empty variable
// counts as unset (the usual way to "clear" one).
const char* env_value(const char* name) {
    if (!name) {
        return nullptr;
    }
    return legacy::env(name);
}

#ifdef _WIN32
// Paths are read through the wide API: getenv() hands back the ANSI code page,
// which turns a user name with an umlaut into a path that does not exist.
fs::path wide_env_path(const wchar_t* name) {
    const wchar_t* value = _wgetenv(name);
    return (value && *value) ? fs::path(value) : fs::path();
}
#endif

// The folder is kept under its old name (loomAI) for a person who has one and has no folder under the new name yet:
// the settings and the secrets file in it stay where they are.
fs::path with_old_folder(const fs::path& current, const fs::path& old) {
    std::error_code ec;
    if (!fs::exists(current, ec) && fs::exists(old, ec)) {
        return old;
    }
    return current;
}

fs::path directory_path() {
#ifdef _WIN32
    if (auto dir = wide_env_path(L"AI_DESKTOP_CONFIG_DIR"); !dir.empty()) {
        return dir;
    }
    if (auto dir = wide_env_path(L"LOOMAI_CONFIG_DIR"); !dir.empty()) {
        return dir;
    }
    if (auto appdata = wide_env_path(L"APPDATA"); !appdata.empty()) {
        return with_old_folder(appdata / "ai-desktop", appdata / "loomai");
    }
    if (auto profile = wide_env_path(L"USERPROFILE"); !profile.empty()) {
        const fs::path roaming = profile / "AppData" / "Roaming";
        return with_old_folder(roaming / "ai-desktop", roaming / "loomai");
    }
    return fs::path("ai-desktop-config");
#else
    if (const char* dir = env_value("AI_DESKTOP_CONFIG_DIR")) {
        return fs::path(dir);
    }
    if (const char* xdg = env_value("XDG_CONFIG_HOME")) {
        return with_old_folder(fs::path(xdg) / "ai-desktop", fs::path(xdg) / "loomai");
    }
    if (const char* home = env_value("HOME")) {
#ifdef __APPLE__
        const fs::path base = fs::path(home) / "Library" / "Application Support";
#else
        const fs::path base = fs::path(home) / ".config";
#endif
        return with_old_folder(base / "ai-desktop", base / "loomai");
    }
    return fs::path("ai-desktop-config");
#endif
}

fs::path file_path() {
    return directory_path() / "config.json";
}

json fresh_doc() {
    return json{{"version", kVersion}, {"settings", json::object()}};
}

// Caller holds g_mutex.
void load_locked() {
    g_loaded = true;
    g_doc = fresh_doc();

    const fs::path path = file_path();
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return;
    }

    // Read in its own scope: Windows refuses to rename a file that is still open,
    // and the corrupt-file path below renames it.
    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        // Never overwrite what we cannot read: it may be the user's only copy.
        fs::path aside = path;
        aside += ".corrupt-" + std::to_string(static_cast<long long>(std::time(nullptr)));
        fs::rename(path, aside, ec);
        std::cerr << "ai-desktop: " << path.u8string() << " is not valid JSON; "
                  << (ec ? "could not move it aside" : "moved it to " + aside.u8string())
                  << ", using defaults\n";
        return;
    }

    g_doc = std::move(parsed);
    if (!g_doc.contains("settings") || !g_doc["settings"].is_object()) {
        g_doc["settings"] = json::object();
    }
    if (!g_doc.contains("version")) {
        g_doc["version"] = kVersion;
    }
}

void ensure_loaded_locked() {
    if (!g_loaded) {
        load_locked();
    }
}

// Caller holds g_mutex. Temp file + rename, so a crash mid-write never leaves a
// half-written config.json behind.
Status save_locked() {
    const fs::path path = file_path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        return {500, "cannot create config directory: " + ec.message()};
    }

    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return {500, "cannot write " + tmp.u8string()};
        }
        out << g_doc.dump(2) << "\n";
        out.flush();
        if (!out) {
            return {500, "failed while writing " + tmp.u8string()};
        }
    }

    fs::rename(tmp, path, ec);
    if (ec) {
        // Some platforms refuse to rename over an existing file.
        std::error_code ignored;
        fs::remove(path, ignored);
        fs::rename(tmp, path, ec);
    }
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return {500, "cannot replace " + path.u8string() + ": " + ec.message()};
    }
    return {};
}

std::string trimmed(const std::string& text) {
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// Validates and normalises one setting value in place.
Status validate(const std::string& key, std::string& value) {
    if (key == "theme") {
        if (value != "system" && value != "light" && value != "dark") {
            return {400, "theme: must be one of system, light, dark"};
        }
    } else if (key == "language") {
        if (value != "de" && value != "en") {
            return {400, "language: must be one of de, en"};
        }
    } else if (key == "default_model") {
        value = trimmed(value);
        if (value.size() > kMaxModelNameLength) {
            return {400, "default_model: longer than 200 characters"};
        }
        for (unsigned char c : value) {
            if (c < 0x20 || c == 0x7f) {
                return {400, "default_model: must not contain control characters"};
            }
        }
    } else if (key == "ollama_host") {
        if (!normalize_http_origin(value)) {
            return {400, "ollama_host: expected an http(s) address like http://127.0.0.1:11434"};
        }
    } else if (key == "ollama_num_ctx") {
        value = trimmed(value);
        int parsed = 0;
        if (!value.empty() && !parse_num_ctx(value, parsed)) {
            return {400, "ollama_num_ctx: expected a whole number of tokens from 0 (send none) to 1048576"};
        }
    }
    return {};
}

// Validates one entry of a "prices" patch: {"input_per_mtok": n, "output_per_mtok": n}.
Status validate_price(const std::string& model, const json& entry) {
    if (model.empty() || model.size() > kMaxModelNameLength) {
        return {400, "prices: a model name must have 1 to 200 characters"};
    }
    for (unsigned char c : model) {
        if (c < 0x20 || c == 0x7f) {
            return {400, "prices: a model name must not contain control characters"};
        }
    }
    if (!entry.is_object()) {
        return {400, "prices." + model + ": expected an object or null"};
    }
    for (const char* key : {"input_per_mtok", "output_per_mtok"}) {
        if (!entry.contains(key) || !entry[key].is_number()) {
            return {400, "prices." + model + ": '" + key + "' (number) is required"};
        }
        if (entry[key].get<double>() < 0) {
            return {400, "prices." + model + ": '" + key + "' must not be negative"};
        }
    }
    for (const auto& item : entry.items()) {
        if (item.key() != "input_per_mtok" && item.key() != "output_per_mtok") {
            return {400, "prices." + model + ": unknown field '" + item.key() + "'"};
        }
    }
    return {};
}

// Applies {"prices": <raw>} to `settings` in place: null drops the stored table, an
// object merges model by model (an entry set to null drops that model).
Status apply_prices(json& settings, const json& raw) {
    if (raw.is_null()) {
        settings.erase("prices");
        return {};
    }
    if (!raw.is_object()) {
        return {400, "prices: expected an object of {\"<model>\": {\"input_per_mtok\", \"output_per_mtok\"}}"};
    }
    json table = (settings.contains("prices") && settings["prices"].is_object()) ? settings["prices"] : json::object();
    for (const auto& item : raw.items()) {
        if (item.value().is_null()) {
            table.erase(item.key());
            continue;
        }
        Status checked = validate_price(item.key(), item.value());
        if (!checked.ok()) {
            return checked;
        }
        table[item.key()] = {{"input_per_mtok", item.value()["input_per_mtok"].get<double>()},
                             {"output_per_mtok", item.value()["output_per_mtok"].get<double>()}};
    }
    if (table.empty()) {
        settings.erase("prices");
    } else {
        settings["prices"] = std::move(table);
    }
    return {};
}

}  // namespace

bool normalize_http_origin(std::string& url) {
    url = trimmed(url);
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }

    std::string rest;
    if (url.rfind("http://", 0) == 0) {
        rest = url.substr(7);
    } else if (url.rfind("https://", 0) == 0) {
        rest = url.substr(8);
    } else {
        return false;
    }
    if (rest.empty()) {
        return false;
    }
    // An origin only: no path, query, fragment or credentials, no whitespace.
    for (unsigned char c : rest) {
        if (c <= 0x20 || c == 0x7f || c == '/' || c == '?' || c == '#' || c == '@' || c == '\\') {
            return false;
        }
    }

    std::string host;
    std::string port;
    if (rest[0] == '[') {
        size_t close = rest.find(']');
        if (close == std::string::npos || close == 1) {
            return false;
        }
        host = rest.substr(1, close - 1);
        std::string tail = rest.substr(close + 1);
        if (!tail.empty()) {
            if (tail[0] != ':' || tail.size() == 1) {
                return false;
            }
            port = tail.substr(1);
        }
        for (unsigned char c : host) {
            if (!std::isxdigit(c) && c != ':' && c != '.') {
                return false;
            }
        }
    } else {
        size_t colon = rest.rfind(':');
        if (colon == std::string::npos) {
            host = rest;
        } else {
            host = rest.substr(0, colon);
            port = rest.substr(colon + 1);
            if (port.empty()) {
                return false;
            }
        }
        if (host.empty()) {
            return false;
        }
        for (unsigned char c : host) {
            if (!std::isalnum(c) && c != '.' && c != '-' && c != '_') {
                return false;
            }
        }
    }

    if (!port.empty()) {
        if (port.size() > 5) {
            return false;
        }
        for (unsigned char c : port) {
            if (c < '0' || c > '9') {
                return false;
            }
        }
        int number = std::stoi(port);
        if (number < 1 || number > 65535) {
            return false;
        }
    }
    return true;
}

std::string directory() {
    return directory_path().u8string();
}

std::string effective(const std::string& key) {
    const SettingDef* def = find_setting(key);
    if (!def) {
        return "";
    }
    if (const char* from_env = env_value(def->env)) {
        return from_env;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_loaded_locked();
    const json& stored = g_doc["settings"];
    if (stored.contains(key) && stored[key].is_string()) {
        return stored[key].get<std::string>();
    }
    return def->fallback;
}

int ollama_num_ctx() {
    int parsed = 0;
    if (parse_num_ctx(trimmed(effective("ollama_num_ctx")), parsed)) {
        return parsed;
    }
    // Only an environment value can be malformed here (a stored one was validated
    // on write): fall back to the built-in default rather than sending none.
    const SettingDef* def = find_setting("ollama_num_ctx");
    parse_num_ctx(def->fallback, parsed);
    return parsed;
}

json settings_json() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_loaded_locked();
    json out = json::object();
    for (const auto& def : kSettings) {
        json entry;
        entry["default"] = def.fallback;
        if (const char* from_env = env_value(def.env)) {
            entry["value"] = from_env;
            entry["source"] = "env";
            entry["locked"] = true;
            entry["env"] = def.env;
        } else if (g_doc["settings"].contains(def.key) && g_doc["settings"][def.key].is_string()) {
            entry["value"] = g_doc["settings"][def.key];
            entry["source"] = "config";
            entry["locked"] = false;
        } else {
            entry["value"] = def.fallback;
            entry["source"] = "default";
            entry["locked"] = false;
        }
        out[def.key] = std::move(entry);
    }
    return out;
}

Status update_settings(const json& patch) {
    if (!patch.is_object()) {
        return {400, "expected a JSON object of settings"};
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_loaded_locked();

    json next = g_doc;
    for (const auto& item : patch.items()) {
        const std::string& key = item.key();
        const json& raw = item.value();
        if (key == "prices") {
            Status applied = apply_prices(next["settings"], raw);
            if (!applied.ok()) {
                return applied;
            }
            continue;
        }
        const SettingDef* def = find_setting(key);
        if (!def) {
            return {400, "unknown setting '" + key + "'"};
        }
        if (env_value(def->env)) {
            return {409, key + " is set by the environment variable " + def->env + " and cannot be changed here"};
        }
        if (raw.is_null()) {
            next["settings"].erase(key);
            continue;
        }
        if (!raw.is_string()) {
            return {400, key + ": expected a string"};
        }
        std::string value = raw.get<std::string>();
        Status checked = validate(key, value);
        if (!checked.ok()) {
            return checked;
        }
        if (value.empty()) {
            // An empty default model means "use the built-in default", so store
            // nothing rather than a value that would make requests name no model.
            next["settings"].erase(key);
            continue;
        }
        next["settings"][key] = value;
    }

    json before = std::move(g_doc);
    g_doc = std::move(next);
    Status saved = save_locked();
    if (!saved.ok()) {
        g_doc = std::move(before);
    }
    return saved;
}

json default_prices() {
    return builtin_prices();
}

json prices() {
    json table = builtin_prices();
    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_loaded_locked();
    const json& settings = g_doc["settings"];
    if (settings.contains("prices") && settings["prices"].is_object()) {
        for (const auto& item : settings["prices"].items()) {
            // Tolerant of a hand-edited file like the rest of the loader: only a
            // row that would pass PUT /settings overlays the default.
            if (validate_price(item.key(), item.value()).ok()) {
                table[item.key()] = item.value();
            }
        }
    }
    return table;
}

json snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_loaded_locked();
    return g_doc;
}

void inspect(const std::function<void(const json&)>& read) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_loaded_locked();
    read(g_doc);
}

Status mutate(const std::function<Status(json&)>& edit) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_loaded_locked();

    json copy = g_doc;
    Status result = edit(copy);
    if (!result.ok()) {
        return result;
    }

    json before = std::move(g_doc);
    g_doc = std::move(copy);
    Status saved = save_locked();
    if (!saved.ok()) {
        g_doc = std::move(before);
    }
    return saved;
}

void reload() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_loaded = false;
}

}  // namespace ai_desktop::config
