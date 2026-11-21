// A stand-in for the tasks command of tasks.nvim, for the tests of ai-desktop (never installed).
//
//   fake_tasks agent-brief --format=json --by=<name> [--repo=<dir>]
//                                                      a brief of the form tasks-agent-brief/1 (brief 2); with --repo
//                                                      (a folder, else exit 2) its `checks`; each call is appended
//                                                      as {args} to $FAKE_TASKS_BRIEF_LOG
//   fake_tasks ci commit <sha>... --recall --root=<dir> --json=<file>
//                                                      as testing.nvim: copies $FAKE_TESTING_RECALL to <file>
//   fake_tasks ci remote --json <dir>                 as testing.nvim's testing command: the file $FAKE_TESTING_REMOTE
//                                                      (exit $FAKE_TESTING_REMOTE_EXIT), the call in $FAKE_TESTING_LOG
//   fake_tasks echo-args <args...>                     each argument on a line of its own (the quoting test)
//   fake_tasks echo-env <NAME>                         the value of a variable
//   fake_tasks sleep <ms>                              waits (the timeout test)
//   fake_tasks <anything else>                         appends {args, cwd, vault} as a JSON line to $FAKE_TASKS_LOG
//
// FAKE_TASKS_BRIEF_VERSION / FAKE_TASKS_CONTRACT set the versions of the brief; FAKE_TASKS_NO_BRIEF=1 answers
// agent-brief as an old engine does (usage error); FAKE_TASKS_FOREIGN_CLAIM=1 refuses a claim as taken.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

namespace {

std::string env(const char* name, const std::string& fallback = "") {
    const char* value = std::getenv(name);
    return value ? value : fallback;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        std::cerr << "usage: fake_tasks <command>\n";
        return 2;
    }
    if (args[0] == "echo-args") {
        for (std::size_t i = 1; i < args.size(); ++i) std::cout << args[i] << "\n";
        return 0;
    }
    if (args[0] == "echo-env" && args.size() == 2) {
        std::cout << env(args[1].c_str()) << "\n";
        return 0;
    }
    if (args[0] == "sleep" && args.size() == 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(args[1].c_str())));
        return 0;
    }
    if (args[0] == "agent-brief") {
        if (env("FAKE_TASKS_NO_BRIEF") == "1") {
            std::cerr << "tasks: unknown command 'agent-brief'\n";
            return 2;
        }
        std::string by = "ai-desktop:<job>";
        std::string repo;
        for (const auto& a : args) {
            if (a.rfind("--by=", 0) == 0) by = a.substr(5);
            if (a.rfind("--repo=", 0) == 0) repo = a.substr(7);
        }
        if (const std::string log = env("FAKE_TASKS_BRIEF_LOG"); !log.empty()) {
            std::ofstream out(std::filesystem::u8path(log), std::ios::app | std::ios::binary);
            out << nlohmann::json({{"args", args}}).dump() << "\n";
        }
        std::error_code ec;
        if (!repo.empty() && !std::filesystem::is_directory(std::filesystem::u8path(repo), ec)) {
            std::cerr << "tasks: --repo is no folder: " << repo << "\n";
            return 2;
        }
        nlohmann::ordered_json doc;
        doc["brief_version"] = std::atoi(env("FAKE_TASKS_BRIEF_VERSION", "2").c_str());
        doc["by"] = by;
        if (!repo.empty()) {
            // The checks section of brief 2 (tasks.nvim agent-brief --repo), for a repository with the testing tools.
            doc["checks"] = {{"source", "testing"},
                             {"land", false},
                             {"per_commit", "testing ci commit <sha> --quick -- --cached"},
                             {"before_push", "testing ci commit HEAD --base=origin/main -- --cached"},
                             {"sync", "git pull --rebase origin main"},
                             {"review", "git log origin/main..HEAD"},
                             {"push", "git push origin HEAD:main"}};
        }
        doc["contract_schema"] = std::atoi(env("FAKE_TASKS_CONTRACT", "1").c_str());
        doc["engine_version"] = "fake-0";
        doc["markdown"] = "# Tasks family (fake)\n\nClaim first: `tasks claim <id> --by=" + by +
                          "`; finish: `tasks done <id> --by=" + by + "`.\n" +
                          (repo.empty() ? "" : "\n## Checks (--repo=" + repo + ")\n");
        doc["plugin"] = "tasks.nvim";
        doc["schema"] = "tasks-agent-brief/1";
        std::cout << doc.dump() << "\n";
        return 0;
    }
    if (args[0] == "ci" && args.size() >= 2 && args[1] == "commit") {
        // As `testing ci commit <sha>... --recall --root=<dir> --json=<file>`: copies $FAKE_TESTING_RECALL there.
        if (const std::string log = env("FAKE_TESTING_LOG"); !log.empty()) {
            std::ofstream out(std::filesystem::u8path(log), std::ios::app | std::ios::binary);
            out << nlohmann::json({{"args", args}}).dump() << "\n";
        }
        std::string json_file;
        bool recall = false;
        for (const auto& a : args) {
            if (a.rfind("--json=", 0) == 0) json_file = a.substr(7);
            recall = recall || a == "--recall";
        }
        const std::string file = env("FAKE_TESTING_RECALL");
        std::ifstream in(std::filesystem::u8path(file), std::ios::binary);
        if (!recall || json_file.empty() || file.empty() || !in) {
            std::cerr << "testing: ci commit is only a stand-in for --recall here\n";
            return 2;
        }
        std::ofstream(std::filesystem::u8path(json_file), std::ios::binary) << in.rdbuf();
        return 0;
    }
    if (args[0] == "ci" && args.size() >= 2 && args[1] == "remote") {
        // As the testing command of testing.nvim: the document of $FAKE_TESTING_REMOTE (a file), exit
        // $FAKE_TESTING_REMOTE_EXIT (ci remote exits 1 for a red CI and prints the document all the same).
        if (const std::string log = env("FAKE_TESTING_LOG"); !log.empty()) {
            std::ofstream out(std::filesystem::u8path(log), std::ios::app | std::ios::binary);
            out << nlohmann::json({{"args", args}}).dump() << "\n";
        }
        const std::string file = env("FAKE_TESTING_REMOTE");
        std::ifstream in(std::filesystem::u8path(file), std::ios::binary);
        if (file.empty() || !in) {
            std::cerr << "testing: no CI adapter for this repository\n";
            return 2;
        }
        std::cout << in.rdbuf();
        return std::atoi(env("FAKE_TESTING_REMOTE_EXIT", "0").c_str());
    }
    if (args[0] == "claim" && env("FAKE_TASKS_FOREIGN_CLAIM") == "1") {
        std::cerr << args[1] << " is claimed by somebody since 2026-11-17T10:00:00Z until 2026-11-17T14:00:00Z\n";
        return 1;
    }
    const std::string log = env("FAKE_TASKS_LOG");
    if (!log.empty()) {
        nlohmann::json line = {{"args", args},
                               {"cwd", std::filesystem::current_path().u8string()},
                               {"vault", env("TASKS_VAULT")}};
        std::ofstream out(std::filesystem::u8path(log), std::ios::app | std::ios::binary);
        out << line.dump() << "\n";
    }
    std::cout << args[0] << "\t" << (args.size() > 1 ? args[1] : "") << "\n";
    return 0;
}
