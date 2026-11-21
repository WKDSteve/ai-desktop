#include "worktree.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>

#include "jobs.hpp"
#include "nlohmann/json.hpp"
#include "process.hpp"

using json = nlohmann::json;

namespace ai_desktop::worktree {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxGitOut = 256 * 1024;

// One removal at a time: the end of a run and a sweep (at the end of ai-desktop) may reach the same worktree.
std::mutex g_mutex;

fs::path git_program() {
#ifdef _WIN32
    return process::find_on_path({"git.exe"});
#else
    return process::find_on_path({"git"});
#endif
}

std::string trimmed(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

std::string first_line(const std::string& text) {
    const std::string all = trimmed(text);
    const std::string line = all.substr(0, all.find_first_of("\r\n"));
    return jobs::sanitize(line.substr(0, 200));
}

struct Git {
    int code = -3;
    std::string out;
    std::string error;
    bool ok() const { return code == 0; }
};

// git in `dir` (`-C`), never asking for anything (no terminal prompt, no credential window), and without a GIT_DIR
// or GIT_WORK_TREE ai-desktop itself might have been started with.
Git git(const fs::path& program, const fs::path& dir, std::vector<std::string> args, std::chrono::seconds timeout) {
    Git result;
    if (program.empty()) {
        result.error = "git is not on PATH";
        return result;
    }
    process::Spec spec;
    spec.program = program;
    spec.args = {"-C", dir.u8string()};
    spec.args.insert(spec.args.end(), args.begin(), args.end());
    spec.env = {{"GIT_TERMINAL_PROMPT", "0"}, {"GCM_INTERACTIVE", "never"}, {"GIT_DIR", ""},
                {"GIT_WORK_TREE", ""},       {"GIT_INDEX_FILE", ""}};
    result.code = process::run_capture(spec, timeout, kMaxGitOut, result.out, &result.error);
    return result;
}

fs::path record_of(const fs::path& root, const std::string& name) {
    return root / fs::u8path(name + ".json");
}

bool write_record(const fs::path& file, const Worktree& worktree) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << json({{"repo", worktree.repo.u8string()}, {"dir", worktree.dir.u8string()}, {"branch", worktree.branch}})
               .dump();
    return static_cast<bool>(out);
}

bool read_record(const fs::path& file, Worktree& out) {
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("repo") || !doc["repo"].is_string() ||
        !doc.contains("dir") || !doc["dir"].is_string() || !doc.contains("branch") || !doc["branch"].is_string()) {
        return false;
    }
    out.repo = fs::u8path(doc["repo"].get<std::string>());
    out.dir = fs::u8path(doc["dir"].get<std::string>());
    out.branch = doc["branch"].get<std::string>();
    return true;
}

}  // namespace

std::string name_of(const std::string& job_id, unsigned long long run) {
    return "job-" + job_id + "-" + std::to_string(run);
}

bool create(const fs::path& root, const fs::path& repo, const std::string& name, Worktree& out, std::string* note,
            std::string* error) {
    const fs::path program = git_program();
    if (program.empty()) {
        if (error) *error = "git is not on PATH: no worktree for the run";
        return false;
    }
    std::error_code ec;
    fs::create_directories(root, ec);
    Worktree worktree{repo, root / fs::u8path(name), "ai-desktop/" + name};
    const fs::path record = record_of(root, name);
    if (!write_record(record, worktree)) {
        if (error) *error = "cannot write " + record.u8string();
        return false;
    }
    const Git fetch = git(program, repo, {"fetch", "--quiet", "--no-tags", "origin", "main"}, std::chrono::seconds(60));
    if (!fetch.ok() && note) {
        *note = "git fetch origin main failed (" + first_line(fetch.error) + "): the worktree starts at the last known "
                "origin/main";
    }
    const Git base = git(program, repo, {"rev-parse", "--verify", "--quiet", "origin/main^{commit}"},
                         std::chrono::seconds(20));
    if (!base.ok()) {
        fs::remove(record, ec);
        if (error) *error = "the repository " + repo.u8string() + " has no origin/main: no worktree for the run";
        return false;
    }
    const Git add = git(program, repo,
                        {"worktree", "add", "--quiet", "-b", worktree.branch, "--track", worktree.dir.u8string(),
                         "origin/main"},
                        std::chrono::seconds(120));
    if (!add.ok()) {
        if (error) *error = "git worktree add failed: " + first_line(add.error);
        // A half-made worktree or branch (git ended in between) goes now; what stays, the next sweep finds.
        remove(root, worktree);
        return false;
    }
    out = std::move(worktree);
    return true;
}

namespace {

// The paths of `git status --porcelain` (one per line, `XY path`; a rename's new path).
std::vector<std::string> status_paths(const std::string& porcelain) {
    std::vector<std::string> paths;
    std::size_t pos = 0;
    while (pos < porcelain.size()) {
        const std::size_t eol = std::min(porcelain.find('\n', pos), porcelain.size());
        std::string line = porcelain.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 3) {
            std::string path = line.substr(3);
            if (const std::size_t arrow = path.find(" -> "); arrow != std::string::npos) path = path.substr(arrow + 4);
            paths.push_back(path);
        }
    }
    return paths;
}

// Saves what the agent left uncommitted as a commit on the job's branch (decision of 2026-11-21): staged inside the
// worktree only, so .gitignore holds (no ignored file, no .env), committed the way every commit of the person is
// (signed when their config says so, with the repository's hooks: no --no-gpg-sign, no --no-verify). Never pushed.
// "" and `sha`/`count` (0 when there was nothing), or why it could not be saved.
std::string save_leftovers(const fs::path& program, const Worktree& worktree, const std::string& when,
                           std::string& sha, long& count) {
    const Git status = git(program, worktree.dir, {"status", "--porcelain", "--untracked-files=all"},
                           std::chrono::seconds(60));
    if (!status.ok()) {
        return "git status failed: " + first_line(status.error);
    }
    if (status_paths(status.out).empty()) {
        return "";
    }
    const Git add = git(program, worktree.dir, {"add", "--all", "--", "."}, std::chrono::seconds(120));
    if (!add.ok()) {
        return "git add failed: " + first_line(add.error);
    }
    const Git staged = git(program, worktree.dir, {"diff", "--cached", "--name-only"}, std::chrono::seconds(60));
    if (!staged.ok()) {
        return "git diff --cached failed: " + first_line(staged.error);
    }
    std::vector<std::string> files;
    {
        std::size_t pos = 0;
        while (pos < staged.out.size()) {
            const std::size_t eol = std::min(staged.out.find('\n', pos), staged.out.size());
            const std::string file = trimmed(staged.out.substr(pos, eol - pos));
            pos = eol + 1;
            if (!file.empty()) files.push_back(file);
        }
    }
    if (files.empty()) {
        return "";  // only what .gitignore keeps out
    }
    const std::string name = worktree.dir.filename().u8string();  // job-<id>-<run>
    const std::string job = name.size() > 4 ? name.substr(4, name.rfind('-') - 4) : name;
    std::string body = "Files:\n";
    for (std::size_t i = 0; i < files.size() && i < 200; ++i) {
        body += "- " + files[i] + "\n";
    }
    if (files.size() > 200) {
        body += "- ... and " + std::to_string(files.size() - 200) + " more\n";
    }
    body += "\nNobody checked these changes: the agent left them uncommitted, and ai-desktop saved them as they were "
            "at " + when + " so they are not lost. Not pushed; not on main.";
    const Git commit = git(program, worktree.dir,
                           {"commit", "--quiet", "-m", "wip(ai-desktop job " + job + "): uncommitted changes at " + when,
                            "-m", body},
                           std::chrono::seconds(300));
    if (!commit.ok()) {
        return "git commit failed: " + first_line(commit.error.empty() ? commit.out : commit.error);
    }
    const Git head = git(program, worktree.dir, {"rev-parse", "--short=12", "HEAD"}, std::chrono::seconds(20));
    sha = trimmed(head.out);
    count = static_cast<long>(files.size());
    return "";
}

}  // namespace

Removal remove(const fs::path& root, const Worktree& worktree, const std::string& when) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Removal result;
    const fs::path record = record_of(root, worktree.dir.filename().u8string());
    std::error_code ec;
    if (!fs::exists(record, ec)) {
        return result;  // already removed
    }
    result.done = true;
    // Only a folder of the worktrees themselves, never one a broken or forged record names elsewhere: the removal
    // below may take a folder by hand.
    if (worktree.dir.parent_path().lexically_normal() != root.lexically_normal() ||
        worktree.branch != "ai-desktop/" + worktree.dir.filename().u8string()) {
        result.error = "the record names no worktree of ai-desktop; left as it is";
        return result;
    }
    const fs::path program = git_program();
    if (program.empty()) {
        result.error = "git is not on PATH";
        return result;
    }
    if (fs::is_directory(worktree.dir, ec)) {
        if (const std::string why = save_leftovers(program, worktree, when, result.wip, result.saved); !why.empty()) {
            // Nothing is thrown away that could not be saved: the worktree stays for a look, and so does its record.
            result.error = "the uncommitted changes could not be saved (" + why + "); the worktree stays";
            return result;
        }
        git(program, worktree.repo, {"worktree", "remove", "--force", "--force", worktree.dir.u8string()},
            std::chrono::seconds(120));
        if (fs::exists(worktree.dir, ec)) {
            // git refused (a file still open, a folder it does not know as a worktree): the folder goes by hand.
            fs::remove_all(worktree.dir, ec);
        }
        if (fs::exists(worktree.dir, ec)) {
            result.error = "cannot remove " + worktree.dir.u8string();
            return result;
        }
    }
    git(program, worktree.repo, {"worktree", "prune"}, std::chrono::seconds(60));
    // Commits of the branch that no remote-tracking ref has: never thrown away. A push of the agent updated
    // origin/main (or the branch `testing ci land` pushed) in the shared refs, so pushed work counts as there.
    const Git unpushed = git(program, worktree.repo,
                             {"rev-list", "--count", "refs/heads/" + worktree.branch, "--not", "--remotes"},
                             std::chrono::seconds(60));
    if (unpushed.ok()) {
        result.unpushed = std::strtol(trimmed(unpushed.out).c_str(), nullptr, 10);
        if (result.unpushed == 0) {
            const Git del = git(program, worktree.repo, {"branch", "-D", "--quiet", worktree.branch},
                                std::chrono::seconds(60));
            if (!del.ok()) {
                result.error = "git branch -D " + worktree.branch + " failed: " + first_line(del.error);
            }
        }
    }
    // The branch is gone or kept on purpose, the folder is gone: the record has done its work.
    fs::remove(record, ec);
    return result;
}

std::vector<fs::path> records(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return out;
    }
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".json") {
            out.push_back(it->path());
        }
    }
    return out;
}

std::vector<std::string> sweep(const fs::path& root, const std::string& when) {
    return sweep(root, records(root), when);
}

std::vector<std::string> sweep(const fs::path& root, const std::vector<fs::path>& records, const std::string& when) {
    std::vector<std::string> lines;
    for (const auto& file : records) {
        std::error_code ec;
        if (!fs::exists(file, ec)) {
            continue;  // its run removed it since the snapshot
        }
        Worktree worktree;
        if (!read_record(file, worktree) || worktree.dir.filename().u8string() + ".json" != file.filename().u8string()) {
            lines.push_back("unreadable record " + file.u8string() + " left as it is");
            continue;
        }
        const Removal removal = remove(root, worktree, when);
        if (removal.done) {
            lines.push_back(trimmed(describe(worktree, removal)));
        }
    }
    return lines;
}

std::string describe(const Worktree& worktree, const Removal& removal) {
    if (!removal.error.empty()) {
        return "▸ worktree " + worktree.dir.u8string() + ": " + removal.error + " (the next start tries again)\n";
    }
    std::string text = "▸ worktree " + worktree.dir.u8string() + " removed";
    if (!removal.wip.empty()) {
        text += ", " + std::to_string(removal.saved) + " uncommitted file(s) saved first as the unchecked WIP commit " +
                removal.wip;
    }
    if (removal.unpushed > 0) {
        text += "; " + std::to_string(removal.unpushed) + " commit(s) on no remote stay on the branch " +
                worktree.branch + " of " + worktree.repo.u8string();
    } else {
        text += "; the branch " + worktree.branch + " is deleted (nothing on it that no remote has)";
    }
    return text + "\n";
}

}  // namespace ai_desktop::worktree
