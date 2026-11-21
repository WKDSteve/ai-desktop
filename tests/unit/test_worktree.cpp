#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "minitest.hpp"
#include "worktree.hpp"

namespace fs = std::filesystem;
namespace worktree = ai_desktop::worktree;

namespace {

bool mentions(const std::vector<std::string>& lines, const std::string& text) {
    return std::any_of(lines.begin(), lines.end(),
                       [&](const std::string& line) { return line.find(text) != std::string::npos; });
}

}  // namespace

TEST(worktree_names_carry_the_job_and_its_run) {
    CHECK_EQ(worktree::name_of("0123456789abcdef", 3), std::string("job-0123456789abcdef-3"));
}

TEST(worktree_sweep_takes_only_the_records_of_its_snapshot) {
    // The start of ai-desktop lists the records before it takes requests; a run that starts while the sweep works
    // writes a record after the snapshot, and the sweep must never take that one.
    const fs::path root = minitest::make_temp_dir("worktree-sweep");
    std::ofstream(root / "job-old-1.json") << "not json";
    const auto snapshot = worktree::records(root);
    CHECK_EQ(snapshot.size(), static_cast<std::size_t>(1));
    std::ofstream(root / "job-new-1.json") << "not json";
    CHECK_EQ(worktree::records(root).size(), static_cast<std::size_t>(2));
    const auto lines = worktree::sweep(root, snapshot);
    CHECK(mentions(lines, "job-old-1.json"));
    CHECK(!mentions(lines, "job-new-1.json"));
    // A record its run removed since the snapshot is skipped without a word.
    fs::remove(root / "job-old-1.json");
    CHECK(worktree::sweep(root, snapshot).empty());
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

TEST(worktree_removal_leaves_a_folder_outside_the_worktrees_alone) {
    const fs::path base = minitest::make_temp_dir("worktree-outside");
    const fs::path root = base / "job-worktrees";
    fs::create_directories(root);
    const fs::path victim = base / "job-x-1";
    fs::create_directories(victim);
    std::ofstream(victim / "keep.txt") << "keep";
    std::ofstream(root / "job-x-1.json") << "{}";
    const worktree::Worktree forged{base / "repo", victim, "ai-desktop/job-x-1"};
    const worktree::Removal removal = worktree::remove(root, forged);
    CHECK(removal.done);
    CHECK(!removal.error.empty());
    CHECK(fs::exists(victim / "keep.txt"));
    CHECK(fs::exists(root / "job-x-1.json"));
    std::error_code ignored;
    fs::remove_all(base, ignored);
}
