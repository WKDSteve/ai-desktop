// Starts a program for a job without a shell or a window, with its whole process tree ended together with it.

#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// A program ai-desktop starts for a job (docs/PROCESSES.md): Claude Code headless, the tasks command for its brief.
// Never through a shell, never with a window: on Windows CreateProcessW with CREATE_NO_WINDOW, the child and all it
// starts in a job object that ends with it (a cancel, or ai-desktop ending, takes the whole tree), and only its three
// pipe handles inherited (not the server's socket). Elsewhere fork/exec into a process group of its own.
namespace ai_desktop::process {

struct Spec {
    std::filesystem::path program;                            // absolute path of the executable
    std::vector<std::string> args;                            // UTF-8, without the program
    std::filesystem::path cwd;                                // empty: ai-desktop's own
    std::vector<std::pair<std::string, std::string>> env;     // set (or, with an empty value, remove) in a copy
    std::string stdin_text;                                   // written, then stdin is closed
};

class Child {
public:
    virtual ~Child() = default;
    // Calls `on_line` with every line of stdout (without its line end) until stdout ends. Lines longer than
    // `max_line` bytes are cut there. Stdout ends at the latest when the child itself ends: then everything it
    // started is ended too, so a descendant holding the pipe cannot keep this waiting.
    virtual void read_lines(const std::function<void(const std::string&)>& on_line, std::size_t max_line) = 0;
    // Waits for the end; the exit code (-1 when it was killed or is unknown).
    virtual int wait() = 0;
    // Ends the child and everything it started. Safe from another thread, also twice.
    virtual void kill() = 0;
    // The last bytes of stderr (at most 8 KiB), complete once wait() returned.
    virtual std::string stderr_tail() = 0;
};

// Starts the program; nullptr and `error` when it cannot.
std::unique_ptr<Child> start(const Spec& spec, std::string* error);

// Runs the program to its end with a time limit: stdout (at most `max_out` bytes kept) and the exit code; -2 when it
// had to be killed after `timeout`, -3 when it did not start (`error`).
int run_capture(const Spec& spec, std::chrono::milliseconds timeout, std::size_t max_out, std::string& out,
                std::string* error);

// The names of the variables of this process's environment.
std::vector<std::string> env_names();

// The first of `names` in a folder of PATH (in order), or empty.
std::filesystem::path find_on_path(const std::vector<std::string>& names);

// Windows: one argument quoted for CommandLineToArgvW (and the C runtime). Exposed for the unit tests.
std::string quote_windows_arg(const std::string& arg);

}  // namespace ai_desktop::process
