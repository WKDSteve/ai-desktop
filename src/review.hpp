#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "jobs.hpp"

// A job of the kind `review-done` (docs/JOBS.md "Reviews"): a finished task is checked against its acceptance. The
// agent only reads: ai-desktop gives it the task, the diffs of the commits the task names (it runs `git show` itself,
// bounded) and read access to the vault and those repositories; the answer ends with a block between the markers
// below, which tasks.nvim (and later tasks-desktop) offers to add to the task as a section, after a yes. Nothing is
// written by the agent.
namespace ai_desktop::review {

constexpr const char* kKind = "review-done";
constexpr const char* kBegin = "<!-- review:begin -->";
constexpr const char* kEnd = "<!-- review:end -->";

// A diff is cut at this size, all of them together at the second.
constexpr std::size_t kMaxDiffBytes = 48 * 1024;
constexpr std::size_t kMaxDiffsBytes = 160 * 1024;
constexpr std::size_t kMaxCommits = 20;

struct Commit {
    std::string repo;  // [A-Za-z0-9._-], not "." or ".."
    std::string sha;   // 7-40 hex digits
};

// The `commits:` of the task's front matter: `[repo@sha, ...]` or a block list of `- repo@sha`. Entries of another
// form are skipped; at most kMaxCommits.
std::vector<Commit> commits_of(const std::string& markdown);

// `<vault>/../<repo>` when it is a git repository (it has a `.git`), else empty.
std::filesystem::path repo_dir(const std::string& vault, const std::string& repo);

// The diffs as Markdown: per commit a heading `### repo@sha` and `git show --stat --patch` in a ```diff block, cut at
// the limits above (said so), or a line why there is none (no repository here, no git, an unknown commit).
std::string diffs(const std::string& vault, const std::vector<Commit>& commits);

// The prompt of a review: what to do, the answer's form (with the markers), the task, the diffs.
std::string prompt(const jobs::Request& request, const std::string& diffs, const std::string& job_id);

// The system text of a one-answer review (a model without tools).
std::string system();

// The folders the agent may read besides its working folder: the vault and the repositories of the commits.
std::vector<std::filesystem::path> read_dirs(const std::string& vault, const std::vector<Commit>& commits);

// The arguments of `claude` for a review: print mode, stream-json, read-only tools (Read, Glob, Grep), the folders to
// read, the model when one is named. The prompt goes on stdin.
std::vector<std::string> claude_args(const std::vector<std::filesystem::path>& dirs, const std::string& model);

// The tools of a review run (in --allowedTools), and the ones refused even where the person's settings allow them
// (in --disallowedTools: no shell, no edit, no web).
const std::vector<std::string>& allowed_tools();
const std::vector<std::string>& disallowed_tools();

}  // namespace ai_desktop::review
