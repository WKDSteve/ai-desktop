// A stand-in for Claude Code headless (`claude -p --output-format stream-json ...`), for the tests of ai-desktop
// (never installed). It does what the agent of a job should do: reads the prompt on stdin, finds the task and the
// claim name in it, runs `tasks claim` and `tasks done` (the `tasks` on its PATH, as the real agent's Bash would) and
// writes stream-json lines as Claude Code does.
//
//   $FAKE_CLAUDE_LOG        a JSON line {args, cwd, path0, vault, prompt} is appended (what ai-desktop passed)
//   $FAKE_CLAUDE_PID_FILE   its process id is written there
//   $FAKE_CLAUDE_SLEEP_MS   waits that long after the claim (the cancel test), and starts a child that waits too
//   $FAKE_CLAUDE_FAIL=1     ends with an error result
//   $FAKE_CLAUDE_LEAVE_MS   before its result, starts `tasks sleep <ms>` in the background with stdout and stderr
//                           inherited and does not wait for it (a descendant that keeps the output pipes open)
//   $FAKE_CLAUDE_DIRTY=1    after the claim, leaves half.txt and .env uncommitted in its folder
//   $FAKE_CLAUDE_SUMMARY    the summary block of the last message: unset = a good one; none, paths, long, partial,
//                           open = one that is missing, unclean, too long, lacks a heading or has no end marker
//   $FAKE_CLAUDE_COMMIT=1   after the claim, writes agent.txt, stages everything (`git add -A`, as a careless agent
//                           would) and commits; then `git push origin HEAD:main`, unless $FAKE_CLAUDE_NO_PUSH=1
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
#include <spawn.h>
#include <unistd.h>
extern char** environ;
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

// Starts `tasks sleep <ms>` (the tasks on PATH) without waiting: it inherits stdout and stderr and outlives us.
void leave_behind(const std::string& ms) {
#ifdef _WIN32
    _spawnlp(_P_NOWAIT, "tasks", "tasks", "sleep", ms.c_str(), nullptr);
#else
    pid_t pid = 0;
    std::string a0 = "tasks", a1 = "sleep", a2 = ms;
    char* argv[] = {a0.data(), a1.data(), a2.data(), nullptr};
    posix_spawnp(&pid, "tasks", nullptr, nullptr, argv, environ);
#endif
}

// The summary block the prompt asks for (docs/JOBS.md "Summary of a hand-over"), as $FAKE_CLAUDE_SUMMARY says:
// unset = a good one, "none" = no block, "paths" = a good one with absolute paths, "long" = over the limit,
// "partial" = a heading missing, "open" = no end marker.
std::string summary_block(const std::string& id) {
    const std::string mode = env("FAKE_CLAUDE_SUMMARY");
    if (mode == "none") {
        return "";
    }
    std::string text = "<!-- summary:begin -->\n## Getan\n\nDid the work of " + id + ".\n\n";
    if (mode == "paths") {
        text += "Edited C:\\Users\\bob\\repo\\a.txt and /home/bob/repo/b.txt and \\\\host\\share\\c.txt, "
                "in ai-desktop/src and https://example.org/home/x.\n\n";
    }
    if (mode == "long") {
        for (int i = 0; i < 200; ++i) {
            text += "A long line of the summary that goes on and on.\n";
        }
        text += "\n";
    }
    text += "## Commits\n\n- demo@0123456789ab the work\n\n";
    if (mode != "partial") {
        text += "## Pr\xC3\xBC" "fungen\n\n- `ci commit` CLEAN\n\n";
    }
    text += "## Offen\n\nnichts\n";
    if (mode != "open") {
        text += "<!-- summary:end -->";
    }
    return text;
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

    // A draft (kind draft-task): no tools, one answer with the draft block.
    if (prompt.find("<!-- draft:begin -->") != std::string::npos) {
        const std::string block = "<!-- draft:begin -->\n```json\n"
                                  "{\"title\": \"Knopf blau machen\", \"area\": \"demo\", \"kind\": \"task\", "
                                  "\"prio\": 2, \"effort\": \"S\", \"value\": 3, \"tags\": [\"ui\", \"made-up\"], "
                                  "\"category\": [\"ux\"], \"plan\": \"\", \"summary\": \"Der Knopf wird blau.\", "
                                  "\"acceptance\": [\"Der Knopf ist blau\"]}\n```\n<!-- draft:end -->";
        emit({{"type", "assistant"},
              {"message", {{"content", json::array({{{"type", "text"}, {"text", block}}})}}}});
        emit({{"type", "result"}, {"subtype", "success"}, {"is_error", false}, {"result", block}});
        return 0;
    }
    // A plan to split (kind split-plan): read only, answer with the JSON drafts.
    if (prompt.find("<!-- split:begin -->") != std::string::npos) {
        const std::string block =
            "<!-- split:begin -->\n"
            "{\"phases\": [{\"name\": \"build\", \"new\": false}, {\"name\": \"polish\", \"new\": true}],\n"
            " \"tasks\": [{\"title\": \"Build the thing\", \"area\": \"demo\", \"kind\": \"feature\", \"prio\": 2, "
            "\"effort\": \"S\", \"value\": 4, \"phase\": \"build\", \"summary\": \"It is built.\", "
            "\"acceptance\": [\"it runs\"], \"after\": [], \"blocked_by\": []}]}\n"
            "<!-- split:end -->";
        emit({{"type", "assistant"},
              {"message", {{"content", json::array({{{"type", "text"}, {"text", "Split it.\n\n" + block}}})}}}});
        emit({{"type", "result"}, {"subtype", "success"}, {"is_error", false}, {"result", block}});
        return 0;
    }
    // A decision (kind prepare-decision): read only, answer with the decision block.
    if (prompt.find("<!-- decision:begin -->") != std::string::npos) {
        const std::string block = "<!-- decision:begin -->\n## Vorschlag\n\n2026-11-18, the stand-in: take the file\n\n"
                                  "### Optionen\n\n1. **A file**: simple. Folgen: no server.\n"
                                  "2. **A database**: more. Folgen: a server to run.\n\n"
                                  "### Empfehlung\n\nA file, since it is enough.\n\n### Offene Annahmen\n\n- one user\n\n"
                                  "## Offene Fragen\n\n### Which store? [q1]\n- [ ] A file (empfohlen)\n- [ ] A database\n"
                                  "<!-- decision:end -->";
        emit({{"type", "assistant"},
              {"message", {{"content", json::array({{{"type", "text"}, {"text", "Weighed it.\n\n" + block}}})}}}});
        emit({{"type", "result"}, {"subtype", "success"}, {"is_error", false}, {"result", block}});
        return 0;
    }
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
    if (env("FAKE_CLAUDE_DIRTY") == "1") {
        // Work left uncommitted: a file of the task and a secret that .gitignore keeps out.
        std::ofstream(std::filesystem::path("half.txt")) << "half done\n";
        std::ofstream(std::filesystem::path(".env")) << "TOKEN=secret\n";
    }
    if (env("FAKE_CLAUDE_COMMIT") == "1") {
        std::ofstream(std::filesystem::path("agent.txt")) << "the work of " << id << "\n";
        tool("git add -A");
        tool("git commit -q -m \"feat: the work of the agent\"");
        if (env("FAKE_CLAUDE_NO_PUSH") != "1") {
            tool("git push -q origin HEAD:main");
        }
    }
    emit({{"type", "assistant"}, {"message", {{"content", json::array({{{"type", "text"}, {"text", "Did the work of " + id + ".\n\n" + summary_block(id)}}})}}}});
    if (env("FAKE_CLAUDE_FAIL") == "1") {
        emit({{"type", "result"}, {"subtype", "success"}, {"is_error", true}, {"result", "boom"}});
        return 1;
    }
    tool("tasks done " + id + " --done-in=fake --by=" + by);
    if (const std::string leave = env("FAKE_CLAUDE_LEAVE_MS"); !leave.empty()) {
        leave_behind(leave);
    }
    emit({{"type", "result"},
          {"subtype", "success"},
          {"is_error", false},
          {"result", "finished"},
          {"usage", {{"input_tokens", 12}, {"output_tokens", 34}}}});
    return 0;
}
