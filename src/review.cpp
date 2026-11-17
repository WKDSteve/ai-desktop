#include "review.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

#include "process.hpp"

namespace ai_desktop::review {

namespace fs = std::filesystem;

namespace {

std::string trimmed(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

bool valid_repo(const std::string& repo) {
    if (repo.empty() || repo.size() > 100 || repo == "." || repo == "..") {
        return false;
    }
    return std::all_of(repo.begin(), repo.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_' || c == '-';
    });
}

bool valid_sha(const std::string& sha) {
    return sha.size() >= 7 && sha.size() <= 40 &&
           std::all_of(sha.begin(), sha.end(), [](unsigned char c) { return std::isxdigit(c); });
}

// Adds `repo@sha` when it is one.
void take(const std::string& word, std::vector<Commit>& out) {
    const std::string item = trimmed(word);
    const std::size_t at = item.find('@');
    if (at == std::string::npos || out.size() >= kMaxCommits) {
        return;
    }
    Commit commit{item.substr(0, at), item.substr(at + 1)};
    std::transform(commit.sha.begin(), commit.sha.end(), commit.sha.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (valid_repo(commit.repo) && valid_sha(commit.sha)) {
        out.push_back(std::move(commit));
    }
}

}  // namespace

std::vector<Commit> commits_of(const std::string& markdown) {
    std::vector<Commit> out;
    std::string text = markdown;
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    if (text.rfind("---\n", 0) != 0) {
        return out;
    }
    const std::size_t end = text.find("\n---", 3);
    const std::string front = text.substr(4, end == std::string::npos ? std::string::npos : end - 3);
    std::size_t pos = 0;
    bool in_block = false;
    while (pos < front.size()) {
        const std::size_t eol = std::min(front.find('\n', pos), front.size());
        const std::string line = front.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.rfind("commits:", 0) == 0) {
            const std::string rest = trimmed(line.substr(8));
            if (!rest.empty() && rest.front() == '[') {
                const std::size_t close = rest.find(']');
                const std::string list = rest.substr(1, close == std::string::npos ? std::string::npos : close - 1);
                std::size_t start = 0;
                while (start <= list.size()) {
                    const std::size_t comma = std::min(list.find(',', start), list.size());
                    take(list.substr(start, comma - start), out);
                    start = comma + 1;
                }
                in_block = false;
            } else {
                in_block = rest.empty();
            }
            continue;
        }
        if (in_block) {
            const std::string item = trimmed(line);
            if (item.rfind("- ", 0) == 0) {
                take(item.substr(2), out);
            } else if (!item.empty()) {
                in_block = false;
            }
        }
    }
    return out;
}

fs::path repo_dir(const std::string& vault, const std::string& repo) {
    if (!valid_repo(repo)) {
        return {};
    }
    const fs::path root = fs::u8path(vault);
    std::error_code ec;
    if (!root.is_absolute()) {
        return {};
    }
    const fs::path dir = root.parent_path() / fs::u8path(repo);
    return fs::exists(dir / ".git", ec) ? dir : fs::path();
}

std::string diffs(const std::string& vault, const std::vector<Commit>& commits) {
    if (commits.empty()) {
        return "(The task names no commits.)\n";
    }
#ifdef _WIN32
    const fs::path git = process::find_on_path({"git.exe"});
#else
    const fs::path git = process::find_on_path({"git"});
#endif
    std::string out;
    for (const auto& commit : commits) {
        out += "### " + commit.repo + "@" + commit.sha + "\n\n";
        if (out.size() >= kMaxDiffsBytes) {
            out += "(Not shown: the diffs together reached " + std::to_string(kMaxDiffsBytes / 1024) + " KiB.)\n\n";
            continue;
        }
        const fs::path dir = repo_dir(vault, commit.repo);
        if (dir.empty()) {
            out += "(No repository " + commit.repo + " beside the vault on this machine: not checkable here.)\n\n";
            continue;
        }
        if (git.empty()) {
            out += "(git is not on PATH: no diff.)\n\n";
            continue;
        }
        process::Spec spec;
        spec.program = git;
        spec.args = {"-C",         dir.u8string(), "show",         "--stat",       "--patch",
                     "--no-color", "--no-ext-diff", "--no-textconv", "--format=fuller", commit.sha};
        std::string text;
        std::string error;
        const int code = process::run_capture(spec, std::chrono::seconds(20), kMaxDiffBytes, text, &error);
        if (code != 0) {
            out += "(git show failed for this commit: " + jobs::sanitize(trimmed(error).substr(0, 200)) + ")\n\n";
            continue;
        }
        const bool cut = text.size() >= kMaxDiffBytes;
        out += "```diff\n" + jobs::sanitize(text) + "```\n";
        if (cut) {
            out += "(Cut at " + std::to_string(kMaxDiffBytes / 1024) + " KiB.)\n";
        }
        out += "\n";
    }
    return out;
}

std::string prompt(const jobs::Request& request, const std::string& diff_text, const std::string& job_id) {
    std::string text;
    text += "Check the finished task " + request.task_id + " of the task vault " + request.vault + " (area " +
            request.area + ") against its acceptance criteria. This is review job " + job_id +
            ", handed over by " + request.source + ".\n\n";
    text += "You only read and answer: change no file, no task and no repository. You may read the vault and the "
            "repositories of the commits (Read, Glob, Grep).\n\n";
    text += "For every line of the task's acceptance checklist (the section `## Akzeptanz` or `## Acceptance`) decide "
            "whether it is met, open, or not checkable, each with evidence: a file and line (`path:line`) or a commit "
            "(`repo@sha`), or why it cannot be checked. A box ticked in the task is a claim, not evidence.\n\n";
    text += "Think it through in your answer, then end it with exactly one block from the line `" +
            std::string(kBegin) + "` to the line `" + std::string(kEnd) +
            "`, written in the language of the task, in this form (German labels for a German task):\n\n";
    text += std::string(kBegin) + "\n";
    text += "## Prüfung\n\n";
    text += "<one line: the date, review job " + job_id + ", the verdict in a few words>\n\n";
    text += "- [x] <criterion> — erfüllt: <evidence>\n";
    text += "- [ ] <criterion> — offen: <what is missing, with evidence>\n";
    text += "- [ ] <criterion> — nicht prüfbar: <why>\n\n";
    text += "### Folgetask (Vorschlag)\n\n";
    text += "- title: <a title for the work that is open>\n";
    text += "- summary: <one sentence>\n";
    text += std::string(kEnd) + "\n\n";
    text += "Leave out the section `### Folgetask (Vorschlag)` when nothing is open. In English: `## Review`, "
            "met / open / not checkable, `### Follow-up task (proposal)`.\n\n";
    text += "## The task as it was finished\n\n" + request.markdown + "\n\n";
    text += "## The commits it names, as git shows them\n\n" + diff_text;
    return text;
}

std::string system() {
    return "You are a careful reviewer for the tasks of a task vault. You compare what a finished task promised in "
           "its acceptance criteria with what its commits changed. You never invent evidence: what the diffs and the "
           "task do not show is not checkable.";
}

std::vector<fs::path> read_dirs(const std::string& vault, const std::vector<Commit>& commits) {
    std::vector<fs::path> dirs = {fs::u8path(vault)};
    for (const auto& commit : commits) {
        const fs::path dir = repo_dir(vault, commit.repo);
        if (!dir.empty() && std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) {
            dirs.push_back(dir);
        }
    }
    return dirs;
}

const std::vector<std::string>& allowed_tools() {
    static const std::vector<std::string> tools = {"Read", "Glob", "Grep"};
    return tools;
}

const std::vector<std::string>& disallowed_tools() {
    static const std::vector<std::string> tools = {"Bash",         "Edit",     "Write",    "MultiEdit",
                                                   "NotebookEdit", "WebFetch", "WebSearch"};
    return tools;
}

std::vector<std::string> claude_args(const std::vector<fs::path>& dirs, const std::string& model) {
    std::vector<std::string> args = {"-p",
                                     "--output-format",
                                     "stream-json",
                                     "--verbose",
                                     "--append-system-prompt",
                                     system(),
                                     "--permission-mode",
                                     "default"};
    for (const auto& dir : dirs) {
        args.insert(args.end(), {"--add-dir", dir.u8string()});
    }
    if (!model.empty()) {
        args.insert(args.end(), {"--model", model});
    }
    // Refused even where the person's own settings of Claude Code allow them: a denial wins over an allowance, so the
    // review stays read-only whatever ~/.claude says.
    args.push_back("--disallowedTools");
    for (const auto& tool : disallowed_tools()) {
        args.push_back(tool);
    }
    args.push_back("--allowedTools");
    for (const auto& tool : allowed_tools()) {
        args.push_back(tool);
    }
    return args;
}

}  // namespace ai_desktop::review
