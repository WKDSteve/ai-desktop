#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "minitest.hpp"
#include "process.hpp"
#include "review.hpp"

namespace fs = std::filesystem;
namespace review = ai_desktop::review;

namespace {

std::string ids(const std::vector<review::Commit>& commits) {
    std::string out;
    for (const auto& c : commits) out += c.repo + "@" + c.sha + " ";
    return out;
}

bool has(const std::vector<std::string>& args, const std::string& word) {
    return std::find(args.begin(), args.end(), word) != args.end();
}

}  // namespace

TEST(review_commits_come_from_the_front_matter) {
    CHECK_EQ(ids(review::commits_of("---\ntitle: x\ncommits: [ai-desktop@ab33ada, tasks.nvim@B3B9AA9]\n---\nbody\n")),
             std::string("ai-desktop@ab33ada tasks.nvim@b3b9aa9 "));
    CHECK_EQ(ids(review::commits_of("---\r\ncommits:\r\n  - a@1234567\r\n  - b@abcdef0\r\ndone_in: x\r\n---\r\n")),
             std::string("a@1234567 b@abcdef0 "));
    // Not a commit: a short or non-hex sha, a path in the repo, no @; and nothing outside the front matter.
    CHECK_EQ(ids(review::commits_of("---\ncommits: [a@123, b@zzzzzzz, ../x@1234567, c]\n---\n")), std::string(""));
    CHECK_EQ(ids(review::commits_of("no front matter\ncommits: [a@1234567]\n")), std::string(""));
    CHECK_EQ(ids(review::commits_of("---\ntitle: x\n---\ncommits: [a@1234567]\n")), std::string(""));
    std::string many = "---\ncommits: [";
    for (int i = 0; i < 30; ++i) many += (i ? ", r@" : "r@") + std::string("123456") + std::to_string(i % 10);
    many += "]\n---\n";
    CHECK_EQ(review::commits_of(many).size(), review::kMaxCommits);
}

TEST(review_refs_name_commits_and_paths) {
    // `refs` mixes paths and commits: only the commits are diffs.
    CHECK_EQ(ids(review::refs_of("---\nrefs: [docs/x.md, ai-desktop@a00618c, lua/y.lua]\n---\n", "refs")),
             std::string("ai-desktop@a00618c "));
    CHECK_EQ(ids(review::refs_of("---\ncommits: [a@1234567]\n---\n", "refs")), std::string(""));
}

TEST(decision_prompt_asks_for_options_and_questions) {
    ai_desktop::jobs::Request request;
    request.task_id = "demo/choose";
    request.area = "demo";
    request.vault = "/vault";
    request.source = "tasks.nvim";
    request.markdown = "---\ntitle: Choose\nstatus: decision\n---\n\nWhich store?\n";
    const std::string text = review::decision_prompt(request, "(The task names no commits.)\n", "0123456789abcdef");
    CHECK(text.find(review::kDecisionBegin) != std::string::npos);
    CHECK(text.find(review::kDecisionEnd) != std::string::npos);
    CHECK(text.find("## Vorschlag") != std::string::npos);
    CHECK(text.find("## Offene Fragen") != std::string::npos);
    CHECK(text.find("(empfohlen)") != std::string::npos);
    CHECK(text.find("decide nothing") != std::string::npos);
    CHECK(text.find("Which store?") != std::string::npos);
    CHECK(review::decision_system().find("never decide") != std::string::npos);
}

TEST(review_reads_only) {
    const auto args = review::claude_args({"/vault", "/repos/x"}, "sonnet");
    for (const char* tool : {"Read", "Glob", "Grep", "Bash", "Edit", "Write", "WebFetch"}) {
        CHECK(has(args, tool));
    }
    // Bash, Edit and Write are named, but after --disallowedTools; the allowed list is the three readers, last.
    const auto allowed = std::find(args.begin(), args.end(), "--allowedTools");
    const auto disallowed = std::find(args.begin(), args.end(), "--disallowedTools");
    CHECK(allowed != args.end() && disallowed != args.end() && disallowed < allowed);
    CHECK((std::vector<std::string>(allowed + 1, args.end()) == review::allowed_tools()));
    CHECK(std::find(disallowed, allowed, "Bash") != allowed);
    CHECK(std::find(disallowed, allowed, "Edit") != allowed);
    CHECK(!has(args, "--dangerously-skip-permissions"));
    CHECK(!has(args, "--append-system-prompt-file"));  // no brief: a review claims nothing
    CHECK(std::count(args.begin(), args.end(), "--add-dir") == 2);
}

TEST(review_prompt_asks_for_the_block) {
    ai_desktop::jobs::Request request;
    request.task_id = "demo/x";
    request.area = "demo";
    request.vault = "/vault";
    request.source = "tasks.nvim";
    request.markdown = "---\ntitle: X\n---\n\n## Akzeptanz\n\n- [x] one\n";
    const std::string text = review::prompt(request, "### demo@1234567\n\n```diff\n+x\n```\n", "0123456789abcdef");
    CHECK(text.find(review::kBegin) != std::string::npos);
    CHECK(text.find(review::kEnd) != std::string::npos);
    CHECK(text.find("## Prüfung") != std::string::npos);
    CHECK(text.find("- [x] one") != std::string::npos);
    CHECK(text.find("+x") != std::string::npos);
    CHECK(text.find("change no file") != std::string::npos);
    CHECK(text.find("0123456789abcdef") != std::string::npos);
}

TEST(review_diffs_of_a_real_repository) {
#ifdef _WIN32
    const fs::path git = ai_desktop::process::find_on_path({"git.exe"});
#else
    const fs::path git = ai_desktop::process::find_on_path({"git"});
#endif
    const fs::path root = minitest::make_temp_dir("review-git");
    const fs::path vault = root / "vault";
    fs::create_directories(vault);
    // A repository without git is said so per commit.
    CHECK(review::diffs(vault.u8string(), {}).find("names no commits") != std::string::npos);
    CHECK(review::diffs(vault.u8string(), {{"absent", "1234567"}}).find("No repository absent") != std::string::npos);
    if (git.empty()) {
        std::cout << "    (git not on PATH: the diff part is skipped)\n";
        minitest::remove_tree(root);
        return;
    }
    const fs::path repo = root / "demo";
    fs::create_directories(repo);
    auto run_git = [&](std::vector<std::string> args, std::string* out = nullptr) {
        ai_desktop::process::Spec spec;
        spec.program = git;
        spec.args = {"-C", repo.u8string(), "-c", "user.name=t", "-c", "user.email=t@example.org",
                     "-c", "commit.gpgsign=false"};
        spec.args.insert(spec.args.end(), args.begin(), args.end());
        std::string text;
        const int code = ai_desktop::process::run_capture(spec, std::chrono::seconds(20), 4096, text, nullptr);
        if (out) *out = text;
        return code;
    };
    CHECK_EQ(run_git({"init", "-q"}), 0);
    minitest::write_file(repo / "feature.txt", "the acceptance is met here\n");
    CHECK_EQ(run_git({"add", "feature.txt"}), 0);
    CHECK_EQ(run_git({"commit", "-q", "-m", "feat: the feature"}), 0);
    std::string sha;
    CHECK_EQ(run_git({"rev-parse", "--short=12", "HEAD"}, &sha), 0);
    while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r')) sha.pop_back();

    CHECK(review::repo_dir(vault.u8string(), "demo") == repo);
    CHECK(review::repo_dir(vault.u8string(), "..").empty());
    const std::string text = review::diffs(vault.u8string(), {{"demo", sha}, {"demo", "deadbeef"}});
    CHECK(text.find("### demo@" + sha) != std::string::npos);
    CHECK(text.find("+the acceptance is met here") != std::string::npos);
    CHECK(text.find("feat: the feature") != std::string::npos);
    CHECK(text.find("git show failed") != std::string::npos);  // the unknown commit
    const auto dirs = review::read_dirs(vault.u8string(), {{"demo", sha}, {"demo", "1234567"}, {"absent", "1234567"}});
    CHECK_EQ(dirs.size(), static_cast<size_t>(2));  // the vault and the repository, once each
    // git writes its objects read-only on Windows: a plain remove_all leaves them behind.
    minitest::remove_tree(root);
    CHECK(!fs::exists(root));
}
