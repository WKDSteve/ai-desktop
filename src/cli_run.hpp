// Runs the terminal commands of ai-desktop (start, stop, status, open) against the server and returns their exit
// codes.

#pragma once

#include <string>

#include "cli.hpp"

namespace ai_desktop::cli {

// Runs a command of the terminal (everything but Command::Serve and Command::Invalid) and returns its exit code.
// `self_exe` is the path of this program (what `start` starts in the background), `version` the version it reports.
int run(const Parsed& parsed, const std::string& self_exe, const std::string& version);

// The path of the running executable, or `argv0` made absolute when the system does not tell.
std::string own_executable(const char* argv0);

}  // namespace ai_desktop::cli
