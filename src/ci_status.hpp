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

// The Markdown section of the prompt: the task's `ci:` and, per commit, its hosted CI (`### repo@sha` and one line
// each) or "not checked" with the reason. `testing` is the testing command (with `error` when there is none).
std::string section(const std::string& vault, const std::vector<review::Commit>& commits, const std::string& task_ci,
                    const agent::TasksCommand& testing);

}  // namespace ai_desktop::ci_status
