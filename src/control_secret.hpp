#pragma once

#include <filesystem>
#include <string>

// The control secret of the family (the `control.secret` of appkit, crates/appkit-core/src/registry.rs): 32 random
// bytes as 64 lower-case hex digits and a line feed, in <config dir>/control.secret, readable by the user only (mode
// 0600; on Windows the file is made empty, its rights cut down to the current user with no inherited entry, and only
// then the secret written). A program of the family that may hand over a job reads it and sends
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

// Compares the bearer token of `authorization` ("Bearer <hex>") with `secret` in constant time (for the length of
// the secret). False when either is empty or the header is of another form.
bool matches(const std::string& authorization, const std::string& secret);

}  // namespace ai_desktop::control_secret
