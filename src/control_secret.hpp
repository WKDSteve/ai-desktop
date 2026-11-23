#pragma once

#include <filesystem>
#include <string>

// The control secret of the family (the `control.secret` of appkit, crates/appkit-core/src/registry.rs): 32 random
// bytes as 64 lower-case hex digits and a line feed, in <config dir>/control.secret, readable by the user only (mode
// 0600; on Windows the current user alone with no inherited entry). An old file is removed first and the secret goes
// into a new one that has these rights from its first moment (O_EXCL; on Windows CREATE_NEW with its security
// descriptor), so a handle somebody opened on the old file never sees it. A program of the family that may hand over a job reads it and sends
// `Authorization: Bearer <hex>`. A web page cannot read the file, so it cannot send a job. A new secret is made at
// every start of the server and the file removed when it ends.
namespace ai_desktop::control_secret {

constexpr std::size_t kBytes = 32;

std::filesystem::path path_in(const std::filesystem::path& dir);

// 64 hex digits from the random source of the system; "" when the system gives none.
std::string generate();

// Writes the secret as described above. Returns "" or what went wrong (then no secret is in force).
std::string write(const std::filesystem::path& dir, const std::string& hex);

// Removes the file (and ignores that it is gone).
void remove(const std::filesystem::path& dir);

// The secret in the file, trimmed (at most 4 KiB read); "" when there is none.
std::string read(const std::filesystem::path& dir);

// Removes the file only while it still holds `hex` (trimmed, at most 4 KiB read, as appkit's read_secret): a server
// that started since with the same folder keeps its secret. Returns true when the file was ours and is gone.
bool remove_if_ours(const std::filesystem::path& dir, const std::string& hex);

// Compares the bearer token of `authorization` ("Bearer <hex>") with `secret` in constant time (for the length of
// the secret). False when either is empty or the header is of another form.
bool matches(const std::string& authorization, const std::string& secret);

}  // namespace ai_desktop::control_secret
