#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "family.hpp"
#include "minitest.hpp"

namespace fs = std::filesystem;
namespace family = ai_desktop::family;

namespace {

std::string read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

family::HubRecord sample() {
    family::HubRecord record;
    record.port = 8080;
    record.pid = 4242;
    record.version = "0.1.0";
    record.instance = "0123456789abcdef0123456789abcdef";
    return record;
}

}  // namespace

TEST(the_record_is_byte_for_byte_what_appkit_writes) {
    // serde_json::to_string_pretty of appkit_core::registry::HubRecord, plus the newline appkit adds. appkit reads
    // it with deny_unknown_fields: no other key, this order.
    CHECK(family::record_text(sample()) ==
          "{\n"
          "  \"app\": \"ai-desktop\",\n"
          "  \"port\": 8080,\n"
          "  \"pid\": 4242,\n"
          "  \"version\": \"0.1.0\",\n"
          "  \"instance\": \"0123456789abcdef0123456789abcdef\"\n"
          "}\n");
}

TEST(the_record_is_written_whole_and_without_a_temporary_file_left) {
    const fs::path dir = minitest::make_temp_dir("family") / "config";
    std::string error;
    CHECK(family::write_record(dir, sample(), &error));
    CHECK(error.empty());
    CHECK(read_all(dir / "hub.json") == family::record_text(sample()));
    CHECK(!fs::exists(dir / "hub.json.tmp"));
    // A second start writes over it.
    auto next = sample();
    next.instance = "ffffffffffffffffffffffffffffffff";
    CHECK(family::write_record(dir, next, &error));
    CHECK(read_all(dir / "hub.json") == family::record_text(next));
}

TEST(an_ending_server_removes_only_its_own_record) {
    const fs::path dir = minitest::make_temp_dir("family-remove");
    std::string error;
    CHECK(family::write_record(dir, sample(), &error));
    family::remove_record(dir, "ffffffffffffffffffffffffffffffff");
    CHECK(fs::exists(dir / "hub.json"));
    family::remove_record(dir, sample().instance);
    CHECK(!fs::exists(dir / "hub.json"));
    family::remove_record(dir, sample().instance);  // already gone: nothing happens
}

TEST(the_folder_of_the_record_follows_the_config_variable) {
    // The unit tests run with whatever the machine has; only the variable that wins is set here.
#ifdef _WIN32
    _wputenv_s(L"AI_DESKTOP_CONFIG_DIR", L"C:\\family-test");
    CHECK(family::config_dir() == fs::path(L"C:\\family-test"));
    _wputenv_s(L"AI_DESKTOP_CONFIG_DIR", L"");
#else
    setenv("AI_DESKTOP_CONFIG_DIR", "/family-test", 1);
    CHECK(family::config_dir() == fs::path("/family-test"));
    unsetenv("AI_DESKTOP_CONFIG_DIR");
#endif
    // Without it: the app's folder below the base, never the old loomai one.
    const fs::path dir = family::config_dir();
    if (!dir.empty()) {
        CHECK(dir.filename() == "ai-desktop");
    }
}

TEST(an_instance_is_32_hex_digits_and_new_every_time) {
    const std::string a = family::new_instance();
    const std::string b = family::new_instance();
    CHECK(a.size() == 32);
    CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(a != b);
    CHECK(family::current_pid() != 0);
}
