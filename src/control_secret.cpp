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
// name other groups; appkit measured a sandbox group with the right to change the file).
std::string restrict_to_user(const fs::path& path) {
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
    PACL acl = nullptr;
    if (const DWORD rc = SetEntriesInAclW(1, &access, nullptr, &acl); rc != ERROR_SUCCESS) {
        return "SetEntriesInAcl failed (" + std::to_string(rc) + ")";
    }
    std::wstring name = path.wstring();
    const DWORD rc = SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT,
                                           DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                           nullptr, acl, nullptr);
    LocalFree(acl);
    if (rc != ERROR_SUCCESS) {
        return "SetNamedSecurityInfo failed (" + std::to_string(rc) + ")";
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
#ifdef _WIN32
    {
        std::ofstream empty(path, std::ios::binary | std::ios::trunc);
        if (!empty) {
            return "cannot create " + path.u8string();
        }
    }
    if (std::string error = restrict_to_user(path); !error.empty()) {
        std::error_code ignored;
        fs::remove(path, ignored);
        return "the control secret could not be restricted to you (" + error +
               "); set AI_DESKTOP_CONFIG_DIR to a folder that only you own";
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.flush();
    if (!out) {
        return "cannot write " + path.u8string();
    }
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return "cannot create " + path.u8string();
    }
    // The mode of open() applies to a new file only; an old one is set right here.
    const bool ok = ::fchmod(fd, 0600) == 0 &&
                    ::write(fd, content.data(), content.size()) == static_cast<ssize_t>(content.size());
    ::close(fd);
    if (!ok) {
        return "cannot write " + path.u8string();
    }
#endif
    return "";
}

void remove(const fs::path& dir) {
    std::error_code ignored;
    fs::remove(path_in(dir), ignored);
}

bool remove_if_ours(const fs::path& dir, const std::string& hex) {
    if (hex.empty()) {
        return false;
    }
    std::string text;
    {
        std::ifstream in(path_in(dir), std::ios::binary);
        if (!in) {
            return false;
        }
        char buffer[4097];
        in.read(buffer, sizeof buffer);
        if (in.gcount() > 4096) {
            return false;  // not a file this program wrote
        }
        text.assign(buffer, static_cast<std::size_t>(in.gcount()));
    }
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    const std::string trimmed = begin == std::string::npos ? "" : text.substr(begin, end - begin + 1);
    if (trimmed != hex) {
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
