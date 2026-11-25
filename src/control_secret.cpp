// The control secret of the family: made at every start, kept in a file only the user can read, and checked in
// constant time.

#include "control_secret.hpp"

#include <cstdio>
#include <fstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ai_desktop::control_secret {

namespace fs = std::filesystem;

namespace {

bool random_bytes(unsigned char* out, std::size_t count) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    std::ifstream in("/dev/urandom", std::ios::binary);
    in.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(count));
    return static_cast<std::size_t>(in.gcount()) == count;
#endif
}

#ifdef _WIN32
// The rights of the file: the current user alone, full control, nothing inherited (the folder's inherited list can
// name other groups; appkit measured a sandbox group with the right to change the file). The caller frees `*acl`
// with LocalFree.
std::string user_only_acl(PACL* acl) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return "OpenProcessToken failed (" + std::to_string(GetLastError()) + ")";
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size ? size : 1);
    const bool got = GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != 0;
    CloseHandle(token);
    if (!got) {
        return "GetTokenInformation failed (" + std::to_string(GetLastError()) + ")";
    }
    PSID sid = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    if (const DWORD rc = SetEntriesInAclW(1, &access, nullptr, acl); rc != ERROR_SUCCESS) {
        return "SetEntriesInAcl failed (" + std::to_string(rc) + ")";
    }
    return "";
}

// Creates `path` as a new file that has those rights from its first moment, and writes `content` into it. Never an
// old file: a program that opened it before (the inherited rights of the folder may let it) keeps its handle when the
// rights change afterwards, and would read the secret through it.
std::string create_user_only(const fs::path& path, const std::string& content) {
    PACL acl = nullptr;
    if (std::string error = user_only_acl(&acl); !error.empty()) {
        return "the control secret could not be restricted to you (" + error +
               "); set AI_DESKTOP_CONFIG_DIR to a folder that only you own";
    }
    SECURITY_DESCRIPTOR descriptor;
    const bool described = InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) &&
                           SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) &&
                           SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED);
    if (!described) {
        const DWORD rc = GetLastError();
        LocalFree(acl);
        return "the control secret could not be restricted to you (security descriptor: " + std::to_string(rc) +
               "); set AI_DESKTOP_CONFIG_DIR to a folder that only you own";
    }
    SECURITY_ATTRIBUTES attributes{sizeof attributes, &descriptor, FALSE};
    // No sharing while it is written; CREATE_NEW fails when a file of that name is (still) there.
    const HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD create_error = GetLastError();
    LocalFree(acl);
    if (file == INVALID_HANDLE_VALUE) {
        return "cannot create " + path.u8string() + " (" + std::to_string(create_error) + ")";
    }
    DWORD written = 0;
    const bool wrote = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
                       written == content.size();
    CloseHandle(file);
    if (!wrote) {
        DeleteFileW(path.wstring().c_str());
        return "cannot write " + path.u8string();
    }
    return "";
}
#endif

}  // namespace

fs::path path_in(const fs::path& dir) {
    return dir / "control.secret";
}

std::string generate() {
    unsigned char bytes[kBytes];
    if (!random_bytes(bytes, kBytes)) {
        return "";
    }
    std::string hex;
    hex.reserve(kBytes * 2);
    for (unsigned char b : bytes) {
        char pair[3];
        std::snprintf(pair, sizeof pair, "%02x", b);
        hex += pair;
    }
    return hex;
}

std::string write(const fs::path& dir, const std::string& hex) {
    if (hex.size() != kBytes * 2) {
        return "no secret to write";
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return "cannot create " + dir.u8string() + ": " + ec.message();
    }
    const fs::path path = path_in(dir);
    const std::string content = hex + "\n";
    // An old file goes first (one a crash left, or one somebody planted or holds open): the secret is only ever
    // written into a file made new for it, with its rights from the start.
    if (!fs::remove(path, ec) && ec) {
        return "cannot replace the old " + path.u8string() + ": " + ec.message();
    }
#ifdef _WIN32
    if (std::string error = create_user_only(path, content); !error.empty()) {
        return error;
    }
#else
    // O_EXCL: a new file only, and never through a link that was put in its place meanwhile.
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return "cannot create " + path.u8string();
    }
    // The umask can take bits away from the mode of open(); the file gets exactly 0600.
    const bool ok = ::fchmod(fd, 0600) == 0 &&
                    ::write(fd, content.data(), content.size()) == static_cast<ssize_t>(content.size());
    ::close(fd);
    if (!ok) {
        ::unlink(path.c_str());
        return "cannot write " + path.u8string();
    }
#endif
    return "";
}

void remove(const fs::path& dir) {
    std::error_code ignored;
    fs::remove(path_in(dir), ignored);
}

std::string read(const fs::path& dir) {
    std::string text;
    {
        std::ifstream in(path_in(dir), std::ios::binary);
        if (!in) {
            return "";
        }
        char buffer[4097];
        in.read(buffer, sizeof buffer);
        if (in.gcount() > 4096) {
            return "";  // not a file this program wrote
        }
        text.assign(buffer, static_cast<std::size_t>(in.gcount()));
    }
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return begin == std::string::npos ? "" : text.substr(begin, end - begin + 1);
}

bool remove_if_ours(const fs::path& dir, const std::string& hex) {
    if (hex.empty()) {
        return false;
    }
    if (read(dir) != hex) {
        return false;
    }
    std::error_code ec;
    return fs::remove(path_in(dir), ec) && !ec;
}

bool matches(const std::string& authorization, const std::string& secret) {
    static const std::string kPrefix = "Bearer ";
    if (secret.empty() || authorization.size() != kPrefix.size() + secret.size() ||
        authorization.compare(0, kPrefix.size(), kPrefix) != 0) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < secret.size(); ++i) {
        diff |= static_cast<unsigned char>(authorization[kPrefix.size() + i] ^ secret[i]);
    }
    return diff == 0;
}

}  // namespace ai_desktop::control_secret
