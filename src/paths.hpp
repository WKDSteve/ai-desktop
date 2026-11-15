#pragma once

// Where the server finds the files it serves. The dashboard lies next to the program (`<app>/dashboard`), not in the
// working directory: the server starts from any folder, and a launcher needs no `cd`/`pushd`.

#include <filesystem>

namespace ai_desktop::paths {

// The folder of the running program; empty when the system does not say.
std::filesystem::path exe_dir();

// The dashboard folder: `env_value` (AI_DESKTOP_DASHBOARD_DIR) when it is set and not empty, else `dashboard` in
// `program_dir`. Empty when neither is known: the working directory is never the answer.
std::filesystem::path dashboard_dir(const char* env_value, const std::filesystem::path& program_dir);

}  // namespace ai_desktop::paths
