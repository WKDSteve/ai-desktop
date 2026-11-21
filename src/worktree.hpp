#pragma once

#include <filesystem>
#include <string>
#include <vector>

// The git worktree of one agent run (docs/JOBS.md "Runs"): a hand-over never works in the shared checkout of the
// area's repository (other sessions keep uncommitted work there), but in a worktree of its own, made from
// origin/main on a branch named after the job. When the run ends (done, failed, cancelled) the worktree goes; a
// commit that is on no remote stays on its branch. A record per worktree in the folder of the worktrees lets the
// next start of ai-desktop clean up what an end without cleanup (a crash, a killed process) left behind.
namespace ai_desktop::worktree {

struct Worktree {
    std::filesystem::path repo;  // the shared checkout it belongs to
    std::filesystem::path dir;   // the worktree
    std::string branch;          // its branch, e.g. ai-desktop/job-0123456789abcdef-1
};

// `job-<id>-<run>`: the name of the worktree's folder and record, and (after `ai-desktop/`) of its branch.
std::string name_of(const std::string& job_id, unsigned long long run);

// Makes the worktree `<root>/<name>` of `repo`: `git fetch origin main` (a failure is said in `note`, the last
// known origin/main is used), then `git worktree add -b ai-desktop/<name> --track <dir> origin/main`. The record
// is written first, so a crash in between leaves nothing that the sweep does not find. False and `error` when
// git is missing, the repository has no origin/main, or git refuses.
bool create(const std::filesystem::path& root, const std::filesystem::path& repo, const std::string& name,
            Worktree& out, std::string* note, std::string* error);

struct Removal {
    bool done = false;              // there was something to remove (false: already removed, e.g. by the sweep)
    long unpushed = 0;              // commits of the branch that are on no remote: the branch is kept
    std::string wip;                // the WIP commit of what the agent left uncommitted (short sha), if there was any
    long saved = 0;                 // the files in it
    std::string error;              // a step that failed (the record stays then, the next sweep tries again)
};

// Removes the worktree and its record; deletes the branch unless it has commits that are on no remote. What the agent
// left uncommitted is first saved as a commit on the job's branch (decision of 2026-11-21): staged inside the worktree
// (.gitignore holds), with the subject `wip(ai-desktop job <id>): uncommitted changes at <when>`, the files and "nobody
// checked these" in the body, signed and with the hooks like every commit of the person, never pushed; the branch then
// stays. When that commit fails (a hook, the signature), nothing is removed: the worktree stays and `error` says why.
// Serialised and idempotent: the run's own end and a sweep may meet.
Removal remove(const std::filesystem::path& root, const Worktree& worktree,
               const std::string& when = "the end of the run");

// The records in `root` now (a snapshot: a run that starts later is not in it).
std::vector<std::filesystem::path> records(const std::filesystem::path& root);

// Removes the worktree of each of `records` (what an earlier ai-desktop left behind, or, at the end of this one,
// what its runs did not clean up yet); no other. Returns one line per record for the log.
// `when` names the moment in the subject of a WIP commit.
std::vector<std::string> sweep(const std::filesystem::path& root, const std::vector<std::filesystem::path>& records,
                               const std::string& when = "a sweep of ai-desktop");

// sweep() of every record in `root` now.
std::vector<std::string> sweep(const std::filesystem::path& root, const std::string& when = "a sweep of ai-desktop");

// The text of a removal for the job's output (one line, ends with a line break).
std::string describe(const Worktree& worktree, const Removal& removal);

}  // namespace ai_desktop::worktree
