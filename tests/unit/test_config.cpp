#include <filesystem>

#include "config.hpp"
#include "minitest.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace config = ai_desktop::config;

namespace {

// Points the config store at a fresh directory and clears the variables that
// would lock settings, so every test starts from a known state.
struct Sandbox {
    fs::path dir;
    explicit Sandbox(const std::string& tag) : dir(minitest::make_temp_dir(tag)) {
        minitest::set_env("AI_DESKTOP_CONFIG_DIR", dir.string());
        minitest::set_env("AI_DESKTOP_DEFAULT_MODEL", "");
        minitest::set_env("AI_DESKTOP_OLLAMA_HOST", "");
        config::reload();
    }
    ~Sandbox() {
        minitest::set_env("AI_DESKTOP_DEFAULT_MODEL", "");
        minitest::set_env("AI_DESKTOP_OLLAMA_HOST", "");
        config::reload();
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }
    fs::path file() const { return dir / "config.json"; }
};

}  // namespace

TEST(normalize_http_origin_accepts_plain_origins) {
    struct Case {
        const char* in;
        const char* out;
    };
    const Case good[] = {
        {"http://127.0.0.1:11434", "http://127.0.0.1:11434"},
        {"https://example.com/", "https://example.com"},
        {"  http://localhost  ", "http://localhost"},
        {"http://[::1]:11434", "http://[::1]:11434"},
        {"http://my-host_1.lan:80", "http://my-host_1.lan:80"},
    };
    for (const auto& c : good) {
        std::string url = c.in;
        CHECK(config::normalize_http_origin(url));
        CHECK_EQ(url, std::string(c.out));
    }
}

TEST(normalize_http_origin_rejects_everything_else) {
    for (const char* bad : {"", "ftp://x", "http://", "https://", "127.0.0.1:11434", "http://host:0",
                            "http://host:65536", "http://host:", "http://:80", "http://host/path",
                            "http://host?x=1", "http://user@host", "http://ho st", "http://[::1", "http://host:80a",
                            "javascript:alert(1)", "http://host\\evil"}) {
        std::string url = bad;
        CHECK(!config::normalize_http_origin(url));
    }
}

TEST(defaults_when_nothing_is_stored) {
    Sandbox box("defaults");
    CHECK_EQ(config::effective("theme"), std::string("system"));
    CHECK_EQ(config::effective("language"), std::string("de"));
    CHECK_EQ(config::effective("default_model"), std::string("llama3.1:8b-q5_K_M"));
    CHECK_EQ(config::effective("ollama_host"), std::string("http://127.0.0.1:11434"));
    CHECK_EQ(config::effective("no_such_key"), std::string(""));
    json all = config::settings_json();
    CHECK_EQ(all["theme"]["source"].get<std::string>(), std::string("default"));
    CHECK(!all["theme"]["locked"].get<bool>());
    CHECK(!fs::exists(box.file()));  // reading never creates the file
}

TEST(update_persists_and_survives_a_reload) {
    Sandbox box("persist");
    auto st = config::update_settings({{"theme", "dark"}, {"language", "en"}});
    CHECK(st.ok());
    CHECK_EQ(config::effective("theme"), std::string("dark"));
    CHECK(fs::exists(box.file()));
    CHECK(!fs::exists(box.dir / "config.json.tmp"));  // the temp file is renamed away

    config::reload();  // forces a re-read from disk
    CHECK_EQ(config::effective("theme"), std::string("dark"));
    CHECK_EQ(config::effective("language"), std::string("en"));
    CHECK_EQ(config::settings_json()["theme"]["source"].get<std::string>(), std::string("config"));
}

TEST(update_is_all_or_nothing) {
    Sandbox box("atomic");
    CHECK(config::update_settings({{"theme", "dark"}}).ok());
    auto st = config::update_settings({{"theme", "light"}, {"language", "klingon"}});
    CHECK_EQ(st.http, 400);
    CHECK_EQ(config::effective("theme"), std::string("dark"));  // the valid half was not applied
}

TEST(update_rejects_bad_input) {
    Sandbox box("badinput");
    CHECK_EQ(config::update_settings({{"theme", "purple"}}).http, 400);
    CHECK_EQ(config::update_settings({{"no_such_key", "x"}}).http, 400);
    CHECK_EQ(config::update_settings({{"theme", 5}}).http, 400);
    CHECK_EQ(config::update_settings({{"ollama_host", "ftp://x"}}).http, 400);
    CHECK_EQ(config::update_settings({{"default_model", std::string(201, 'a')}}).http, 400);
    CHECK_EQ(config::update_settings({{"default_model", std::string("a\nb")}}).http, 400);
    CHECK_EQ(config::update_settings(json::array()).http, 400);
    CHECK(!fs::exists(box.file()));  // nothing valid happened, nothing was written
}

TEST(null_and_empty_default_model_reset_to_the_default) {
    Sandbox box("reset");
    CHECK(config::update_settings({{"theme", "dark"}, {"default_model", "mistral:7b"}}).ok());
    CHECK_EQ(config::effective("default_model"), std::string("mistral:7b"));
    CHECK(config::update_settings({{"theme", nullptr}}).ok());
    CHECK_EQ(config::effective("theme"), std::string("system"));
    CHECK(config::update_settings({{"default_model", "  "}}).ok());
    CHECK_EQ(config::effective("default_model"), std::string("llama3.1:8b-q5_K_M"));
}

TEST(ollama_host_is_normalised_on_write) {
    Sandbox box("host");
    CHECK(config::update_settings({{"ollama_host", " http://10.0.0.5:11434/ "}}).ok());
    CHECK_EQ(config::effective("ollama_host"), std::string("http://10.0.0.5:11434"));
}

TEST(ollama_num_ctx_defaults_to_32768) {
    Sandbox box("numctx-default");
    CHECK_EQ(config::effective("ollama_num_ctx"), std::string("32768"));
    CHECK_EQ(config::ollama_num_ctx(), 32768);
}

TEST(ollama_num_ctx_is_stored_trimmed_and_zero_means_none) {
    Sandbox box("numctx-store");
    CHECK(config::update_settings({{"ollama_num_ctx", " 8192 "}}).ok());
    CHECK_EQ(config::effective("ollama_num_ctx"), std::string("8192"));
    CHECK_EQ(config::ollama_num_ctx(), 8192);
    CHECK(config::update_settings({{"ollama_num_ctx", "0"}}).ok());
    CHECK_EQ(config::ollama_num_ctx(), 0);
    CHECK(config::update_settings({{"ollama_num_ctx", "1048576"}}).ok());
    CHECK_EQ(config::ollama_num_ctx(), 1048576);
    // empty resets to the built-in default, like default_model
    CHECK(config::update_settings({{"ollama_num_ctx", ""}}).ok());
    CHECK_EQ(config::ollama_num_ctx(), 32768);
}

TEST(ollama_num_ctx_rejects_what_is_not_a_token_count) {
    Sandbox box("numctx-bad");
    for (const char* bad : {"-1", "abc", "1.5", "1e5", "+8192", "8 192", "1048577", "99999999"}) {
        CHECK_EQ(config::update_settings({{"ollama_num_ctx", bad}}).http, 400);
    }
    CHECK_EQ(config::update_settings({{"ollama_num_ctx", 8192}}).http, 400);  // a string, like every setting
    CHECK(!fs::exists(box.file()));
}

TEST(ollama_num_ctx_from_a_malformed_environment_value_falls_back_to_the_default) {
    Sandbox box("numctx-env");
    minitest::set_env("AI_DESKTOP_OLLAMA_NUM_CTX", "4096");
    CHECK_EQ(config::ollama_num_ctx(), 4096);
    minitest::set_env("AI_DESKTOP_OLLAMA_NUM_CTX", "lots");
    CHECK_EQ(config::ollama_num_ctx(), 32768);
    minitest::set_env("AI_DESKTOP_OLLAMA_NUM_CTX", "");
    CHECK_EQ(config::ollama_num_ctx(), 32768);
}

TEST(ollama_num_ctx_from_a_hand_edited_config_falls_back_to_the_default) {
    Sandbox box("numctx-file");
    minitest::write_file(box.file(), R"({"version": 1, "settings": {"ollama_num_ctx": "a lot"}})");
    config::reload();
    CHECK_EQ(config::ollama_num_ctx(), 32768);
    minitest::write_file(box.file(), R"({"version": 1, "settings": {"ollama_num_ctx": "4096"}})");
    config::reload();
    CHECK_EQ(config::ollama_num_ctx(), 4096);
}

TEST(environment_variable_wins_and_locks_the_setting) {
    Sandbox box("envlock");
    CHECK(config::update_settings({{"default_model", "from-config"}}).ok());
    minitest::set_env("AI_DESKTOP_DEFAULT_MODEL", "from-env");
    CHECK_EQ(config::effective("default_model"), std::string("from-env"));
    json all = config::settings_json();
    CHECK_EQ(all["default_model"]["source"].get<std::string>(), std::string("env"));
    CHECK(all["default_model"]["locked"].get<bool>());
    CHECK_EQ(all["default_model"]["env"].get<std::string>(), std::string("AI_DESKTOP_DEFAULT_MODEL"));
    auto st = config::update_settings({{"default_model", "other"}});
    CHECK_EQ(st.http, 409);
    // The stored value is untouched underneath the lock.
    minitest::set_env("AI_DESKTOP_DEFAULT_MODEL", "");
    CHECK_EQ(config::effective("default_model"), std::string("from-config"));
}

TEST(a_corrupt_file_is_moved_aside_not_overwritten) {
    Sandbox box("corrupt");
    minitest::write_file(box.file(), "{ this is not json");
    config::reload();
    CHECK_EQ(config::effective("theme"), std::string("system"));  // defaults, no crash
    bool found_aside = false;
    for (const auto& entry : fs::directory_iterator(box.dir)) {
        if (entry.path().filename().string().rfind("config.json.corrupt-", 0) == 0) {
            found_aside = true;
            CHECK_EQ(minitest::read_file(entry.path()), std::string("{ this is not json"));
        }
    }
    CHECK(found_aside);
    CHECK(config::update_settings({{"theme", "light"}}).ok());  // and the store works afterwards
    CHECK_EQ(config::effective("theme"), std::string("light"));
}

TEST(unknown_keys_survive_a_round_trip) {
    Sandbox box("future");
    minitest::write_file(box.file(), R"({"version":1,"settings":{"theme":"dark"},"future_feature":{"a":[1,2,3]}})");
    config::reload();
    CHECK_EQ(config::effective("theme"), std::string("dark"));
    CHECK(config::update_settings({{"language", "en"}}).ok());
    json on_disk = json::parse(minitest::read_file(box.file()));
    CHECK(on_disk.contains("future_feature"));
    CHECK_EQ(on_disk["future_feature"]["a"].size(), static_cast<size_t>(3));
}

TEST(mutate_persists_ok_edits_and_drops_failed_ones) {
    Sandbox box("mutate");
    auto ok = config::mutate([](json& doc) {
        doc["custom_models"] = json::array();
        doc["custom_models"].push_back(json{{"id", "m1"}});
        return config::Status{};
    });
    CHECK(ok.ok());
    CHECK_EQ(config::snapshot()["custom_models"].size(), static_cast<size_t>(1));

    auto rejected = config::mutate([](json& doc) {
        doc["custom_models"].push_back(json{{"id", "m2"}});
        return config::Status{400, "no"};
    });
    CHECK_EQ(rejected.http, 400);
    CHECK_EQ(config::snapshot()["custom_models"].size(), static_cast<size_t>(1));  // the failed edit left no trace

    config::reload();
    CHECK_EQ(config::snapshot()["custom_models"].size(), static_cast<size_t>(1));  // and it is on disk
}

TEST(a_directory_that_cannot_be_created_reports_500_and_keeps_the_old_state) {
    Sandbox box("nodir");
    // A *file* where the config directory should be: create_directories fails.
    minitest::write_file(box.dir / "blocker", "x");
    minitest::set_env("AI_DESKTOP_CONFIG_DIR", (box.dir / "blocker" / "sub").string());
    config::reload();
    auto st = config::update_settings({{"theme", "dark"}});
    CHECK_EQ(st.http, 500);
    CHECK_EQ(config::effective("theme"), std::string("system"));  // the failed write was rolled back
    minitest::set_env("AI_DESKTOP_CONFIG_DIR", box.dir.string());
}

// A setting made under the old name (loomAI) keeps working: the variables LOOMAI_*, and the folder of the old name.
TEST(an_old_variable_still_locks_a_setting_and_the_new_one_wins) {
    Sandbox box("oldenv");
    minitest::set_env("LOOMAI_DEFAULT_MODEL", "old-model");
    config::reload();
    CHECK_EQ(config::effective("default_model"), std::string("old-model"));
    CHECK_EQ(config::settings_json()["default_model"]["source"].get<std::string>(), std::string("env"));
    minitest::set_env("AI_DESKTOP_DEFAULT_MODEL", "new-model");
    config::reload();
    CHECK_EQ(config::effective("default_model"), std::string("new-model"));
    minitest::set_env("LOOMAI_DEFAULT_MODEL", "");
    minitest::set_env("AI_DESKTOP_DEFAULT_MODEL", "");
    config::reload();
}

#ifndef _WIN32
TEST(the_folder_of_the_old_name_is_kept_until_the_new_one_exists) {
    const fs::path base = minitest::make_temp_dir("oldfolder");
    minitest::set_env("AI_DESKTOP_CONFIG_DIR", "");
    minitest::set_env("LOOMAI_CONFIG_DIR", "");
    minitest::set_env("XDG_CONFIG_HOME", base.string());
    config::reload();
    CHECK_EQ(fs::path(config::directory()), base / "ai-desktop");  // nothing yet: the new name

    fs::create_directories(base / "loomai");
    config::reload();
    CHECK_EQ(fs::path(config::directory()), base / "loomai");  // only the old folder is there: it stays in use

    fs::create_directories(base / "ai-desktop");
    config::reload();
    CHECK_EQ(fs::path(config::directory()), base / "ai-desktop");  // the new one wins as soon as it exists

    minitest::set_env("LOOMAI_CONFIG_DIR", (base / "elsewhere").string());
    config::reload();
    CHECK_EQ(fs::path(config::directory()), base / "elsewhere");  // the old variable still points the folder

    minitest::set_env("LOOMAI_CONFIG_DIR", "");
    minitest::set_env("XDG_CONFIG_HOME", "");
    std::error_code ignored;
    fs::remove_all(base, ignored);
}
#endif
