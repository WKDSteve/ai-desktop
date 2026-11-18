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

TEST(control_secret_is_removed_only_while_it_is_ours) {
    const fs::path dir = minitest::make_temp_dir("secret-ours");
    const std::string mine = secret::generate();
    const std::string theirs = secret::generate();
    CHECK(!secret::remove_if_ours(dir, mine));  // no file
    CHECK_EQ(secret::write(dir, theirs), std::string(""));  // a server started since with the same folder
    CHECK(!secret::remove_if_ours(dir, mine));
    CHECK(fs::exists(secret::path_in(dir)));
    CHECK(!secret::remove_if_ours(dir, ""));
    CHECK(secret::remove_if_ours(dir, theirs));
    CHECK(!fs::exists(secret::path_in(dir)));
    minitest::write_file(secret::path_in(dir), std::string(5000, 'a'));  // larger than any secret: left alone
    CHECK(!secret::remove_if_ours(dir, std::string(5000, 'a')));
    std::error_code ignored;
    fs::remove_all(dir, ignored);
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

// A program that may open files in the config folder (the folder's inherited rights can name another group) opens
// an old control.secret, or plants one, before the server starts and keeps the handle. Rights set on that file
// afterwards do not take back a handle already open: the secret must go into a new file, never into that one.
TEST(control_secret_never_goes_into_a_file_someone_holds_open) {
    const fs::path dir = minitest::make_temp_dir("secret-held");
    const fs::path path = secret::path_in(dir);
    const std::string hex = secret::generate();
#ifdef _WIN32
    minitest::write_file(path, "");
    HANDLE held = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    const std::string error = secret::write(dir, hex);
    char buffer[256];
    DWORD got = 0;
    SetFilePointer(held, 0, nullptr, FILE_BEGIN);
    CHECK(ReadFile(held, buffer, sizeof buffer, &got, nullptr));
    CHECK_EQ(std::string(buffer, got).find(hex), std::string::npos);
    CloseHandle(held);
#else
    // Elsewhere the same idea through a link: the secret must not follow it into the file it names.
    const fs::path victim = dir / "victim";
    minitest::write_file(victim, "keep me");
    fs::create_symlink(victim, path);
    const std::string error = secret::write(dir, hex);
    CHECK_EQ(minitest::read_file(victim), std::string("keep me"));
#endif
    if (error.empty()) {
        CHECK_EQ(minitest::read_file(path), hex + "\n");
    }
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
