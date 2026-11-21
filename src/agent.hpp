#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "config.hpp"
#include "jobs.hpp"
#include "nlohmann/json.hpp"

// A job of the tasks family run by Claude Code headless (`claude -p`, docs/JOBS.md "Runs"): the model `claude-code`
// (or `claude-code:<model>`, passed on as --model). Its system prompt gets the brief of the tasks family, generated
// by tasks.nvim (`tasks agent-brief --format=json`), so the agent claims, works on and finishes the task with the
// tasks command. Pure logic and the calls of the two programs; the thread of a run is in jobs_api.cpp.
namespace ai_desktop::agent {

constexpr const char* kModel = "claude-code";
// The brief and contract this version of ai-desktop was written against (tasks-agent-brief/1). Brief 2 has the
// checks of a repository (`agent-brief --repo=<dir>`, field `checks`), which the allowed tools are made for.
constexpr int kBriefVersion = 2;
constexpr int kContractSchema = 1;

// `claude-code` or `claude-code:<model>`.
bool is_agent_model(const std::string& model);
// The part after `claude-code:`; empty for `claude-code`.
std::string submodel(const std::string& model);

// How ai-desktop calls the tasks command, and the folder the agent gets on its PATH.
struct TasksCommand {
    std::filesystem::path program;     // nvim (for a tasks.nvim checkout) or the command itself
    std::vector<std::string> prefix;   // arguments before the command's own
    std::filesystem::path bin_dir;     // the folder of `tasks`, put first on the agent's PATH
    std::string error;                 // why there is none
    bool ok() const { return error.empty(); }
};

// The setting `tasks_cli` (a path), else `tasks` on PATH. A tasks.nvim checkout (bin/tasks or bin/tasks.cmd beside
// ../scripts/tasks.lua) runs as `nvim -n -i NONE --headless -u NONE -l <tasks.lua>`, never through cmd.exe or sh;
// any other file must be an executable (.exe on Windows).
TasksCommand find_tasks();

// The same for the testing command of testing.nvim (setting `testing_cli`, else `testing` on PATH): ai-desktop asks
// it, read-only, for the state of the hosted CI of a review's commits (src/ci_status.hpp). The agent never gets it.
TasksCommand find_testing();

// The setting `claude_cli`, else `claude` on PATH (claude.exe on Windows: a .cmd would need a shell). Empty and
// `error` when there is none.
std::filesystem::path find_claude(std::string* error);

struct Brief {
    int brief_version = 0;
    int contract_schema = 0;
    std::string engine_version;
    std::string markdown;
    std::string note;  // e.g. a newer brief than this ai-desktop knows
    std::string checks_source;  // `checks.source` (testing, claude-md, none) of a brief for a repository; else empty
};

// Checks the JSON of `tasks agent-brief --format=json`: schema tasks-agent-brief/1, brief_version and
// contract_schema at least the ones above (an older engine is refused), a Markdown text. 422 with the reason.
config::Status parse_brief(const std::string& text, Brief& out);

// Runs `tasks agent-brief --format=json --by=<by>` (30 s at most), with `--repo=<repo>` when one is given (the
// run's worktree: the brief then has the checks of that repository), and parses it.
config::Status fetch_brief(const TasksCommand& tasks, const std::string& by, Brief& out,
                           const std::filesystem::path& repo = {});

// The folder the agent works in: the repository of the area beside the vault (`<vault>/../<area>` with a `.git`),
// else the vault. Empty and `error` when the vault is not an existing absolute folder.
std::filesystem::path work_dir(const std::string& vault, const std::string& area, std::string* error);

// The arguments of `claude`: print mode, stream-json, the brief appended to the system prompt from a file, only the
// built-in tools of tool_set() and no MCP server, the allowed tools (the tasks command, git, the checks of the CI
// rhythm, read and edit; nothing else is asked for, so in print mode nothing else runs), the vault as an extra
// folder, the model when one is named. The prompt goes on stdin.
std::vector<std::string> claude_args(const std::filesystem::path& brief_file, const std::filesystem::path& vault,
                                     const std::string& model);

// The tools the agent may use (in --allowedTools): a fixed list, never taken from the brief or the repository (a
// CLAUDE.md of a steered stand must not decide which commands run without a question). Beyond `tasks` and `git` the
// commands of the checks of brief 2: `ci commit` (per commit, and before the push) and `ci land`, each through
// `scripts/test.sh` of the repository or the `testing` command.
const std::vector<std::string>& allowed_tools();
// The commands refused although `Bash(git:*)` would allow them (--disallowedTools, which wins over the allowed list;
// decision of 2026-11-21): `git push --no-verify` (past the repository's pre-push hook) and `git -c` (a config value
// for one call, e.g. core.hooksPath or core.sshCommand). A barrier against a mistake, not against a steered agent:
// the rules match the start of the command only (docs/JOBS.md, Threat model).
const std::vector<std::string>& disallowed_tools();
// The built-in tools the agent's session has at all (--tools, comma-separated): the ones of allowed_tools(), no other.
const std::string& tool_set();

// The variables the agent must not inherit, as {name, ""} (removals for process::Spec::env): when ai-desktop was
// itself started from a Claude Code session (CLAUDECODE is set), that session's variables (CLAUDECODE,
// CLAUDE_CODE_*, CLAUDE_AGENT_SDK_*, the host's sign-in: ANTHROPIC_BASE_URL, USE_*_OAUTH, ...) would make the
// agent a child of that session and send it to the host's sign-in. Otherwise nothing is removed.
std::vector<std::pair<std::string, std::string>> session_env_removals(const std::vector<std::string>& names);

// The prompt of a run: the task, its id, the claim name and what to do. `branch` names the branch of the run's own
// git worktree (src/worktree.hpp; empty: the run works in the vault, there is no repository of the area): the
// prompt then says where it works and how it pushes.
std::string prompt(const jobs::Request& request, const std::string& by, const std::string& branch = "");

// What one line of Claude Code's stream-json means for the job.
struct Event {
    std::string text;        // appended to the output
    bool final = false;      // the result line
    bool is_error = false;   // the result is an error
    std::string error;       // its text
    long input_tokens = 0;
    long output_tokens = 0;
};
Event render(const std::string& line);

}  // namespace ai_desktop::agent
