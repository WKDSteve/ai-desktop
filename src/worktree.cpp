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

long count_lines(const std::string& text) {
    long lines = 0;
    for (const char c : text) {
        lines += c == '\n' ? 1 : 0;
    }
    return lines;
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

Removal remove(const fs::path& root, const Worktree& worktree) {
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
        const Git status = git(program, worktree.dir, {"status", "--porcelain", "--untracked-files=all"},
                               std::chrono::seconds(60));
        result.dropped = status.ok() ? count_lines(status.out) : 0;
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

std::vector<std::string> sweep(const fs::path& root) {
    return sweep(root, records(root));
}

std::vector<std::string> sweep(const fs::path& root, const std::vector<fs::path>& records) {
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
        const Removal removal = remove(root, worktree);
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
    if (removal.dropped > 0) {
        text += ", " + std::to_string(removal.dropped) + " uncommitted file(s) dropped with it";
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
