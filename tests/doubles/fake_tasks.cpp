// A stand-in for the tasks command of tasks.nvim, for the tests of ai-desktop (never installed).
//
//   fake_tasks agent-brief --format=json --by=<name>   a brief of the form tasks-agent-brief/1
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
        for (const auto& a : args) {
            if (a.rfind("--by=", 0) == 0) by = a.substr(5);
        }
        nlohmann::ordered_json doc;
        doc["brief_version"] = std::atoi(env("FAKE_TASKS_BRIEF_VERSION", "1").c_str());
        doc["by"] = by;
        doc["contract_schema"] = std::atoi(env("FAKE_TASKS_CONTRACT", "1").c_str());
        doc["engine_version"] = "fake-0";
        doc["markdown"] = "# Tasks family (fake)\n\nClaim first: `tasks claim <id> --by=" + by +
                          "`; finish: `tasks done <id> --by=" + by + "`.\n";
        doc["plugin"] = "tasks.nvim";
        doc["schema"] = "tasks-agent-brief/1";
        std::cout << doc.dump() << "\n";
        return 0;
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
