#pragma once

#include <map>
#include <string>
#include <vector>

#include "agent.hpp"
#include "review.hpp"

// The CI state of a review's commits (docs/JOBS.md "Reviews"): what ai-desktop itself reads, the way it runs
// `git show`, and puts into the prompt. The review agent gets no tool for it. Sources: the task's `ci:`
// (pending, green, red; tasks.nvim), and the hosted CI of each repository through `testing ci remote --json`
// (testing.nvim, read-only: the run of the commit at the head of origin/main). A source that answers nothing is said as
// "not checked", never left out.
namespace ai_desktop::ci_status {

// The `ci:` of the task's front matter: pending, green or red; empty when there is none or another value.
std::string task_ci(const std::string& markdown);

// The hosted CI of one repository, from `testing-ci-remote/1`: the commit at the head of origin/main and the
// state of its CI run (success, failure, running, ...).
struct Remote {
    std::string sha;      // full sha of origin/main's head
    std::string overall;  // the worst state of its jobs, as testing.nvim names it
    std::string url;      // the run, when there is one
};

// Parses the document; false (and `error`) for another schema or form. Takes the first repository.
bool parse_remote(const std::string& text, Remote& out, std::string* error);

// Asks `testing ci remote --json <dir>` (60 s at most).
bool ask_remote(const agent::TasksCommand& testing, const std::string& dir, Remote& out, std::string* error);

// The kept results of `testing ci commit <sha>... --recall` (testing-ci-commit/1 with `recall: true`): per full sha
// one line per kept result (verdict, "(red)" for regression/unclassified/error/infra, quick or full, base, profile,
// system, WSL2) and per green proof of its tree. Only values of the schema's lists and checked words reach a line.
bool parse_recall(const std::string& text, std::map<std::string, std::vector<std::string>>& out, std::string* error);

// Asks `testing ci commit <sha>... --recall --root=<dir> --json=<json_file>` (60 s; it never runs a check and takes
// no turn in the queue) and parses the file, which it removes afterwards.
bool ask_recall(const agent::TasksCommand& testing, const std::string& dir, const std::vector<std::string>& shas,
                const std::filesystem::path& json_file, std::map<std::string, std::vector<std::string>>& out,
                std::string* error);

// The Markdown section of the prompt: the task's `ci:` and, per commit (`### repo@sha`), its kept local results and
// proofs (one --recall per repository, its answer written below `scratch`) and its hosted CI, or "not checked" with
// the reason. `testing` is the testing command (with `error` when there is none).
std::string section(const std::string& vault, const std::vector<review::Commit>& commits, const std::string& task_ci,
                    const agent::TasksCommand& testing, const std::filesystem::path& scratch);

}  // namespace ai_desktop::ci_status
