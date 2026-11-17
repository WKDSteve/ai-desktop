// A stand-in for Claude Code headless (`claude -p --output-format stream-json ...`), for the tests of ai-desktop
// (never installed). It does what the agent of a job should do: reads the prompt on stdin, finds the task and the
// claim name in it, runs `tasks claim` and `tasks done` (the `tasks` on its PATH, as the real agent's Bash would) and
// writes stream-json lines as Claude Code does.
//
//   $FAKE_CLAUDE_LOG        a JSON line {args, cwd, path0, vault, prompt} is appended (what ai-desktop passed)
//   $FAKE_CLAUDE_PID_FILE   its process id is written there
//   $FAKE_CLAUDE_SLEEP_MS   waits that long after the claim (the cancel test), and starts a child that waits too
//   $FAKE_CLAUDE_FAIL=1     ends with an error result
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using json = nlohmann::json;

namespace {

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

void emit(const json& line) {
    std::cout << line.dump() << "\n" << std::flush;
}

void tool(const std::string& command) {
    emit({{"type", "assistant"},
          {"message", {{"content", json::array({{{"type", "tool_use"}, {"name", "Bash"}, {"input", {{"command", command}}}}})}}}});
    const int rc = std::system(command.c_str());
    emit({{"type", "user"},
          {"message", {{"content", json::array({{{"type", "tool_result"}, {"is_error", rc != 0}, {"content", rc == 0 ? "ok" : "failed"}}})}}}});
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    const std::string prompt((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    const std::string path = env("PATH");
#ifdef _WIN32
    const std::string path0 = path.substr(0, path.find(';'));
#else
    const std::string path0 = path.substr(0, path.find(':'));
#endif
    if (const std::string log = env("FAKE_CLAUDE_LOG"); !log.empty()) {
        std::ofstream out(std::filesystem::u8path(log), std::ios::app | std::ios::binary);
        out << json({{"args", args},
                     {"cwd", std::filesystem::current_path().u8string()},
                     {"path0", path0},
                     {"vault", env("TASKS_VAULT")},
                     {"prompt", prompt}})
                   .dump()
            << "\n";
    }
    if (const std::string pid_file = env("FAKE_CLAUDE_PID_FILE"); !pid_file.empty()) {
        std::ofstream(std::filesystem::u8path(pid_file)) << getpid();
    }
    emit({{"type", "system"}, {"subtype", "init"}, {"model", "fake-model"}, {"cwd", std::filesystem::current_path().u8string()}});

    // A review (kind review-done): read only, answer with the block between the markers; no tasks command.
    if (prompt.find("<!-- review:begin -->") != std::string::npos) {
        const std::string block = "<!-- review:begin -->\n## Prüfung\n\n2026-11-18, review by the stand-in: one open\n\n"
                                  "- [x] it is done — erfüllt: feature.txt:1\n"
                                  "- [ ] it is documented — offen: no README change\n\n"
                                  "### Folgetask (Vorschlag)\n\n- title: Document it\n- summary: The README names it.\n"
                                  "<!-- review:end -->";
        emit({{"type", "assistant"},
              {"message", {{"content", json::array({{{"type", "tool_use"}, {"name", "Read"}, {"input", {{"file_path", "feature.txt"}}}}})}}}});
        emit({{"type", "assistant"},
              {"message", {{"content", json::array({{{"type", "text"}, {"text", "Checked it.\n\n" + block}}})}}}});
        emit({{"type", "result"}, {"subtype", "success"}, {"is_error", false}, {"result", block},
              {"usage", {{"input_tokens", 5}, {"output_tokens", 6}}}});
        return 0;
    }

    std::smatch match;
    const std::regex claim_re("`tasks claim ([A-Za-z0-9._/-]+) --by=([^`\\s]+)`");
    if (!std::regex_search(prompt, match, claim_re)) {
        emit({{"type", "result"}, {"subtype", "error_during_execution"}, {"is_error", true}, {"result", "no task in the prompt"}});
        return 1;
    }
    const std::string id = match[1].str();
    const std::string by = match[2].str();

    tool("tasks claim " + id + " --by=" + by);
    if (const std::string sleep = env("FAKE_CLAUDE_SLEEP_MS"); !sleep.empty()) {
        // A grandchild that waits as well: a cancel must end it too (the process tree).
        tool("tasks sleep " + sleep);
    }
    emit({{"type", "assistant"}, {"message", {{"content", json::array({{{"type", "text"}, {"text", "Did the work of " + id + "."}}})}}}});
    if (env("FAKE_CLAUDE_FAIL") == "1") {
        emit({{"type", "result"}, {"subtype", "success"}, {"is_error", true}, {"result", "boom"}});
        return 1;
    }
    tool("tasks done " + id + " --done-in=fake --by=" + by);
    emit({{"type", "result"},
          {"subtype", "success"},
          {"is_error", false},
          {"result", "finished"},
          {"usage", {{"input_tokens", 12}, {"output_tokens", 34}}}});
    return 0;
}
