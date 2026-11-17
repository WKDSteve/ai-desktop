#include <filesystem>
#include <string>

#include "control_secret.hpp"
#include "minitest.hpp"

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
namespace secret = ai_desktop::control_secret;

TEST(control_secret_is_64_hex_digits_and_new_each_time) {
    const std::string a = secret::generate();
    const std::string b = secret::generate();
    CHECK_EQ(a.size(), static_cast<size_t>(64));
    CHECK_EQ(a.find_first_not_of("0123456789abcdef"), std::string::npos);
    CHECK(a != b);
}

TEST(control_secret_round_trips_through_the_file) {
    const fs::path dir = minitest::make_temp_dir("secret") / "nested";
    const std::string hex = secret::generate();
    CHECK_EQ(secret::write(dir, hex), std::string(""));
    CHECK_EQ(minitest::read_file(secret::path_in(dir)), hex + "\n");
    const std::string next = secret::generate();
    CHECK_EQ(secret::write(dir, next), std::string(""));  // an old file is replaced
    CHECK_EQ(minitest::read_file(secret::path_in(dir)), next + "\n");
    CHECK(!secret::write(dir, "short").empty());
    secret::remove(dir);
    CHECK(!fs::exists(secret::path_in(dir)));
    secret::remove(dir);  // gone already: no error
    std::error_code ignored;
    fs::remove_all(dir.parent_path(), ignored);
}

TEST(control_secret_file_is_for_the_user_only) {
    const fs::path dir = minitest::make_temp_dir("secret-rights");
    CHECK_EQ(secret::write(dir, secret::generate()), std::string(""));
#ifdef _WIN32
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    std::wstring name = secret::path_in(dir).wstring();
    CHECK_EQ(GetNamedSecurityInfoW(name.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                                   nullptr, &descriptor),
             static_cast<DWORD>(ERROR_SUCCESS));
    CHECK(dacl != nullptr);
    if (dacl) {
        // One entry, not inherited: the current user.
        CHECK_EQ(static_cast<int>(dacl->AceCount), 1);
        void* ace = nullptr;
        if (GetAce(dacl, 0, &ace)) {
            CHECK((reinterpret_cast<ACE_HEADER*>(ace)->AceFlags & INHERITED_ACE) == 0);
        }
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    CHECK(GetSecurityDescriptorControl(descriptor, &control, &revision));
    CHECK((control & SE_DACL_PROTECTED) != 0);
    LocalFree(descriptor);
#else
    struct stat info {};
    CHECK_EQ(::stat(secret::path_in(dir).c_str(), &info), 0);
    CHECK_EQ(static_cast<int>(info.st_mode & 0777), 0600);
#endif
    std::error_code ignored;
    fs::remove_all(dir, ignored);
}

TEST(control_secret_matches_only_the_exact_bearer) {
    const std::string hex = secret::generate();
    CHECK(secret::matches("Bearer " + hex, hex));
    CHECK(!secret::matches("bearer " + hex, hex));
    CHECK(!secret::matches("Bearer " + hex + " ", hex));
    CHECK(!secret::matches("Bearer " + hex.substr(1), hex));
    std::string other = hex;
    other[63] = other[63] == 'a' ? 'b' : 'a';
    CHECK(!secret::matches("Bearer " + other, hex));
    CHECK(!secret::matches("", hex));
    CHECK(!secret::matches("Bearer ", ""));
    CHECK(!secret::matches(hex, hex));
}
