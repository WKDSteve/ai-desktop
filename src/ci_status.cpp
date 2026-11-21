#include "ci_status.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <initializer_list>
#include <iterator>

#include "jobs.hpp"
#include "nlohmann/json.hpp"
#include "process.hpp"

using json = nlohmann::json;

namespace ai_desktop::ci_status {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxRemoteBytes = 256 * 1024;

std::string trimmed(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

std::string first_line(const std::string& text) {
    const std::string all = trimmed(text);
    return jobs::sanitize(all.substr(0, std::min(all.find_first_of("\r\n"), std::size_t{200})));
}

bool is_hex(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c); });
}

// A state word of testing.nvim (success, failure, running, waiting_runner, ...): lower-case letters and `_` only, so
// nothing of a foreign document reaches the prompt as text.
bool is_state(const std::string& text) {
    return !text.empty() && text.size() <= 32 &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::islower(c) || c == '_'; });
}

// A link of a run on GitHub, nothing else: no Markdown or text of a foreign document in the prompt.
bool is_run_url(const std::string& url) {
    return url.size() <= 300 && url.rfind("https://github.com/", 0) == 0 &&
           std::all_of(url.begin(), url.end(), [](unsigned char c) {
               return std::isalnum(c) || c == '/' || c == ':' || c == '.' || c == '-' || c == '_';
           });
}

fs::path git_program() {
#ifdef _WIN32
    return process::find_on_path({"git.exe"});
#else
    return process::find_on_path({"git"});
#endif
}

// 1: `sha` is an ancestor of (or is) `head` in `dir`; 0: it is not; -1: git could not tell.
int contained(const fs::path& dir, const std::string& sha, const std::string& head) {
    const fs::path git = git_program();
    if (git.empty()) {
        return -1;
    }
    process::Spec spec;
    spec.program = git;
    spec.args = {"-C", dir.u8string(), "merge-base", "--is-ancestor", sha, head};
    spec.env = {{"GIT_TERMINAL_PROMPT", "0"}};
    std::string out;
    std::string error;
    const int code = process::run_capture(spec, std::chrono::seconds(20), 4096, out, &error);
    return code == 0 ? 1 : code == 1 ? 0 : -1;
}

}  // namespace

std::string task_ci(const std::string& markdown) {
    std::string text = markdown;
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    if (text.rfind("---\n", 0) != 0) {
        return "";
    }
    const std::size_t end = text.find("\n---", 3);
    const std::string front = text.substr(4, end == std::string::npos ? std::string::npos : end - 3);
    std::size_t pos = 0;
    while (pos < front.size()) {
        const std::size_t eol = std::min(front.find('\n', pos), front.size());
        const std::string line = front.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.rfind("ci:", 0) != 0) {
            continue;
        }
        std::string value = trimmed(line.substr(3));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }
        return value == "pending" || value == "green" || value == "red" ? value : "";
    }
    return "";
}

bool parse_remote(const std::string& text, Remote& out, std::string* error) {
    const json doc = json::parse(text, nullptr, false);
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("schema") || !doc["schema"].is_string() ||
        doc["schema"].get<std::string>() != "testing-ci-remote/1") {
        return fail("no testing-ci-remote/1 document");
    }
    if (!doc.contains("repos") || !doc["repos"].is_array() || doc["repos"].empty() || !doc["repos"][0].is_object()) {
        return fail("the document names no repository");
    }
    const json& repo = doc["repos"][0];
    if (repo.contains("error") && repo["error"].is_string() && !repo["error"].get<std::string>().empty()) {
        return fail(first_line(repo["error"].get<std::string>()));
    }
    Remote remote;
    if (repo.contains("sha") && repo["sha"].is_string()) {
        remote.sha = repo["sha"].get<std::string>();
    }
    if (repo.contains("overall") && repo["overall"].is_string()) {
        remote.overall = repo["overall"].get<std::string>();
    }
    if (remote.sha.size() != 40 || !is_hex(remote.sha) || !is_state(remote.overall)) {
        return fail("no commit or state of origin/main in the document");
    }
    if (repo.contains("run") && repo["run"].is_object() && repo["run"].contains("url") && repo["run"]["url"].is_string() &&
        is_run_url(repo["run"]["url"].get<std::string>())) {
        remote.url = repo["run"]["url"].get<std::string>();
    }
    out = std::move(remote);
    return true;
}

bool ask_remote(const agent::TasksCommand& testing, const std::string& dir, Remote& out, std::string* error) {
    if (!testing.ok()) {
        if (error) *error = testing.error;
        return false;
    }
    process::Spec spec;
    spec.program = testing.program;
    spec.args = testing.prefix;
    spec.args.insert(spec.args.end(), {"ci", "remote", "--json", dir});
    std::string text;
    std::string run_error;
    const int code = process::run_capture(spec, std::chrono::seconds(60), kMaxRemoteBytes, text, &run_error);
    // A red CI ends `ci remote` with exit 1 and the document all the same: the document decides, not the exit.
    if (code < 0 || trimmed(text).empty()) {
        if (error) *error = "testing ci remote gave no answer (" + std::to_string(code) + "): " + first_line(run_error);
        return false;
    }
    return parse_remote(text, out, error);
}

namespace {

bool one_of(const std::string& value, std::initializer_list<const char*> words) {
    return std::any_of(words.begin(), words.end(), [&](const char* w) { return value == w; });
}

// A profile name of `.testing.lua`: letters, digits, `.`, `_`, `-`.
bool is_word(const std::string& text) {
    return !text.empty() && text.size() <= 32 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_' || c == '-';
    });
}

std::string str(const json& doc, const char* key) {
    return doc.contains(key) && doc[key].is_string() ? doc[key].get<std::string>() : "";
}

std::string on_system(const json& item) {
    const std::string system = str(item, "system");
    std::string text = one_of(system, {"windows", "linux", "macos"}) ? " on " + system : "";
    if (item.contains("from_wsl") && item["from_wsl"].is_boolean() && item["from_wsl"].get<bool>()) {
        text += " (in WSL2)";
    }
    return text;
}

}  // namespace

bool parse_recall(const std::string& text, std::map<std::string, std::vector<std::string>>& out, std::string* error) {
    const json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || str(doc, "schema") != "testing-ci-commit/1" ||
        !doc.contains("recall") || !doc["recall"].is_boolean() || !doc["recall"].get<bool>()) {
        if (error) *error = "no testing-ci-commit/1 document of --recall";
        return false;
    }
    std::map<std::string, std::vector<std::string>> lines;
    if (doc.contains("commits") && doc["commits"].is_array()) {
        for (const auto& item : doc["commits"]) {
            if (!item.is_object()) continue;
            const std::string sha = str(item, "sha");
            const std::string verdict = str(item, "verdict");
            if (sha.size() != 40 || !is_hex(sha) ||
                !one_of(verdict, {"clean", "known-red", "flaky", "regression", "unclassified", "docs-only", "empty",
                                  "nothing-affected", "error", "infra"})) {
                continue;
            }
            const bool quick = item.contains("quick") && item["quick"].is_boolean() && item["quick"].get<bool>();
            const bool red = one_of(verdict, {"regression", "unclassified", "error", "infra"});
            std::string line = "local check (`testing ci commit`): " + verdict + (red ? " (red)" : "");
            line += quick ? ", quick (only what the diff reaches: no proof of the tree)" : ", full";
            const std::string base = str(item, "base");
            if (base.size() >= 7 && base.size() <= 40 && is_hex(base)) {
                line += ", against " + base.substr(0, 12);
            }
            if (const std::string profile = str(item, "profile"); is_word(profile)) {
                line += ", profile " + profile;
            }
            lines[sha].push_back(line + on_system(item));
        }
    }
    if (doc.contains("proofs") && doc["proofs"].is_array()) {
        for (const auto& item : doc["proofs"]) {
            if (!item.is_object()) continue;
            const std::string sha = str(item, "sha");
            const std::string tree = str(item, "tree");
            const std::string profile = str(item, "profile");
            if (sha.size() != 40 || !is_hex(sha) || tree.size() < 12 || !is_hex(tree) || !is_word(profile)) {
                continue;
            }
            lines[sha].push_back("local proof: its tree " + tree.substr(0, 12) + " was green in profile " + profile +
                                 on_system(item));
        }
    }
    out = std::move(lines);
    return true;
}

bool ask_recall(const agent::TasksCommand& testing, const std::string& dir, const std::vector<std::string>& shas,
                const fs::path& json_file, std::map<std::string, std::vector<std::string>>& out, std::string* error) {
    if (!testing.ok()) {
        if (error) *error = testing.error;
        return false;
    }
    std::error_code ec;
    fs::create_directories(json_file.parent_path(), ec);
    fs::remove(json_file, ec);
    process::Spec spec;
    spec.program = testing.program;
    spec.args = testing.prefix;
    spec.args.insert(spec.args.end(), {"ci", "commit"});
    spec.args.insert(spec.args.end(), shas.begin(), shas.end());
    spec.args.insert(spec.args.end(), {"--recall", "--root=" + dir, "--json=" + json_file.u8string()});
    std::string printed;
    std::string run_error;
    const int code = process::run_capture(spec, std::chrono::seconds(60), kMaxRemoteBytes, printed, &run_error);
    std::ifstream in(json_file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    fs::remove(json_file, ec);
    if (code != 0 || text.empty()) {
        if (error) *error = "testing ci commit --recall failed (" + std::to_string(code) + "): " + first_line(run_error);
        return false;
    }
    return parse_recall(text, out, error);
}

std::string section(const std::string& vault, const std::vector<review::Commit>& commits, const std::string& task_ci,
                    const agent::TasksCommand& testing, const fs::path& scratch) {
    std::string text;
    text += task_ci.empty() ? "The task has no `ci:` field (not checked by tasks.nvim).\n\n"
                            : "The task says `ci: " + task_ci + "`.\n\n";
    if (commits.empty()) {
        return text + "(The task names no commits.)\n";
    }
    std::map<std::string, std::pair<bool, Remote>> asked;  // per repository: answered, the state
    std::map<std::string, std::string> why_not;
    // The kept results of `testing ci commit` per repository (--recall: read, never run), by full sha.
    std::map<std::string, std::pair<bool, std::map<std::string, std::vector<std::string>>>> recalled;
    std::map<std::string, std::string> recall_why;
    for (const auto& commit : commits) {
        text += "### " + commit.repo + "@" + commit.sha + "\n\n";
        const fs::path dir = review::repo_dir(vault, commit.repo);
        if (dir.empty()) {
            text += "- local check (`testing ci commit`): not checked (no repository here)\n";
            text += "- hosted CI: not checked (no repository " + commit.repo + " beside the vault on this machine)\n\n";
            continue;
        }
        if (recalled.find(commit.repo) == recalled.end()) {
            std::vector<std::string> shas;
            for (const auto& other : commits) {
                if (other.repo == commit.repo) shas.push_back(other.sha);
            }
            std::map<std::string, std::vector<std::string>> kept;
            std::string error = "no folder for the answer";
            const bool ok = !scratch.empty() &&
                            ask_recall(testing, dir.u8string(), shas, scratch / fs::u8path(commit.repo + ".json"), kept,
                                       &error);
            recalled[commit.repo] = {ok, std::move(kept)};
            recall_why[commit.repo] = error;
        }
        const auto& [recall_ok, kept] = recalled[commit.repo];
        std::vector<std::string> local;
        for (const auto& [sha, lines] : kept) {
            if (sha.rfind(commit.sha, 0) == 0) local.insert(local.end(), lines.begin(), lines.end());
        }
        if (!recall_ok) {
            text += "- local check (`testing ci commit`): not checked (" + jobs::sanitize(recall_why[commit.repo]) + ")\n";
        } else if (local.empty()) {
            text += "- local check (`testing ci commit`): not checked (nothing kept for this commit)\n";
        }
        for (const auto& line : local) {
            text += "- " + line + "\n";
        }
        if (asked.find(commit.repo) == asked.end()) {
            Remote remote;
            std::string error;
            const bool ok = ask_remote(testing, dir.u8string(), remote, &error);
            asked[commit.repo] = {ok, remote};
            why_not[commit.repo] = error;
        }
        const auto& [ok, remote] = asked[commit.repo];
        if (!ok) {
            text += "- hosted CI: not checked (" + jobs::sanitize(why_not[commit.repo]) + ")\n\n";
            continue;
        }
        const std::string head = "origin/main is at " + remote.sha.substr(0, 12) + ", its CI run: " + remote.overall +
                                 (remote.url.empty() ? "" : " (" + remote.url + ")");
        const int in = contained(dir, commit.sha, remote.sha);
        if (in == 1) {
            text += "- hosted CI: " + head + "; this commit is part of it\n\n";
        } else if (in == 0) {
            text += "- hosted CI: not checked (this commit is not on origin/main; " + head + ")\n\n";
        } else {
            text += "- hosted CI: not checked (git cannot tell whether this commit is on origin/main; " + head + ")\n\n";
        }
    }
    return text;
}

}  // namespace ai_desktop::ci_status
