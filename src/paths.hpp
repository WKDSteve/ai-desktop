// Where the server finds the dashboard files it serves: next to the program, never in the working directory.

#pragma once

// Where the server finds the files it serves. The dashboard lies next to the program (`<app>/dashboard`), not in the
// working directory: the server starts from any folder, and a launcher needs no `cd`/`pushd`.

#include <filesystem>
#include <string>

namespace ai_desktop::paths {

// The folder of the running program; empty when the system does not say.
std::filesystem::path exe_dir();

// The name of `path` for narrow file calls (cpp-httplib's stat and ifstream): on Windows in the system's code page,
// else in its 8.3 short form; empty when neither can hold it. Elsewhere the path as it is.
std::string file_api_name(const std::filesystem::path& path);

// The dashboard folder as cpp-httplib takes it: `env_value` (AI_DESKTOP_DASHBOARD_DIR) as it is when it is set and
// not empty, else `dashboard` in `program_dir`. Empty when neither is known or the name cannot be held: the working
// directory is never the answer.
std::string dashboard_dir(const char* env_value, const std::filesystem::path& program_dir);

}  // namespace ai_desktop::paths
