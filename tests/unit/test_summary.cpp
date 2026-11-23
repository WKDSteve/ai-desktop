#include <string>

#include "agent.hpp"
#include "jobs.hpp"
#include "minitest.hpp"
#include "summary.hpp"

namespace agent = ai_desktop::agent;
namespace jobs = ai_desktop::jobs;
namespace summary = ai_desktop::summary;

namespace {

std::string good(const std::string& extra = "") {
    return std::string("Done.\n\n") + summary::kBegin + "\n## Getan\n\nthe work\n" + extra +
           "\n## Commits\n\n- demo@0123456789ab the work\n\n## Prüfungen\n\n- ci commit CLEAN\n\n## Offen\n\nnichts\n" +
           summary::kEnd;
}

}  // namespace

TEST(summary_extract_keeps_the_block_with_job_and_date) {
    const auto result = summary::extract(good(), "0123456789abcdef", "2026-11-23");
    CHECK_EQ(result.error, std::string(""));
    CHECK_EQ(result.summary["schema"].get<std::string>(), std::string("handoff-summary/1"));
    CHECK_EQ(result.summary["job_id"].get<std::string>(), std::string("0123456789abcdef"));
    CHECK_EQ(result.summary["date"].get<std::string>(), std::string("2026-11-23"));
    const std::string md = result.summary["markdown"].get<std::string>();
    CHECK(md.rfind("## Getan", 0) == 0);
    CHECK(md.find("summary:begin") == std::string::npos);
}

TEST(summary_extract_takes_the_last_block) {
    const std::string text = std::string(summary::kBegin) + "\nold\n" + summary::kEnd + "\n" + good();
    CHECK(summary::extract(text, "x", "2026-11-23").error.empty());
}

TEST(summary_extract_refuses_a_missing_or_open_block) {
    CHECK(!summary::extract("no block", "x", "d").error.empty());
    const std::string open = good().substr(0, good().rfind(summary::kEnd));
    CHECK(!summary::extract(open, "x", "d").error.empty());
}

TEST(summary_extract_needs_every_heading_in_order) {
    std::string text = good();
    text.replace(text.find("## Prüfungen"), std::string("## Prüfungen").size(), "## Checks");
    const auto result = summary::extract(text, "x", "d");
    CHECK(result.summary.is_null());
    CHECK(result.error.find("Prüfungen") != std::string::npos);
    // Out of order: Offen before Commits.
    const std::string swapped = std::string(summary::kBegin) +
                                "\n## Getan\n\na\n\n## Offen\n\nb\n\n## Commits\n\nc\n\n## Prüfungen\n\nd\n" + summary::kEnd;
    CHECK(!summary::extract(swapped, "x", "d").error.empty());
    // A heading inside a line is no heading.
    const std::string inline_heading = std::string(summary::kBegin) +
                                       "\n## Getan\n\na ## Commits\n\n## Prüfungen\n\nd\n\n## Offen\n\ne\n" + summary::kEnd;
    CHECK(!summary::extract(inline_heading, "x", "d").error.empty());
}

TEST(summary_extract_is_bounded) {
    std::string filler;
    while (filler.size() < summary::kMaxBytes) {
        filler += "A long line of the summary that goes on and on.\n";
    }
    const auto result = summary::extract(good(filler), "x", "d");
    CHECK(result.summary.is_null());
    CHECK(result.error.find("bytes") != std::string::npos);
}

TEST(summary_clean_replaces_absolute_paths_and_bad_characters) {
    const std::string cleaned =
        summary::clean("a C:\\Users\\bob\\x.txt b D:/work/y c \\\\host\\share\\z d /home/bob/r e `/tmp/q` f");
    CHECK(cleaned.find("bob") == std::string::npos);
    CHECK(cleaned.find("host") == std::string::npos);
    CHECK(cleaned.find("/tmp") == std::string::npos);
    CHECK(cleaned.find("<path>") != std::string::npos);
    // What is no path stays: a task id, a relative file, a URL.
    const std::string kept = summary::clean("ai-desktop/src/jobs.cpp and https://example.org/home/x and and/or");
    CHECK_EQ(kept, std::string("ai-desktop/src/jobs.cpp and https://example.org/home/x and and/or"));
    // The character rule of the jobs: a bell and a zero-width space become U+FFFD.
    const std::string bad = summary::clean(std::string("a\x07") + "b\xE2\x80\x8B" + "c");
    CHECK_EQ(jobs::text_error(bad, true), std::string(""));
    CHECK(bad.find('\x07') == std::string::npos);
}

TEST(summary_is_asked_for_in_the_prompt_of_a_hand_over) {
    jobs::Request request;
    request.source = "tasks-desktop";
    request.task_id = "demo/first";
    request.vault = "/v";
    request.area = "demo";
    request.markdown = "the task";
    const std::string text = agent::prompt(request, "ai-desktop:1");
    CHECK(text.find(summary::kBegin) != std::string::npos);
    CHECK(text.find(summary::kEnd) != std::string::npos);
    for (const char* heading : summary::kHeadings) {
        CHECK(text.find(heading) != std::string::npos);
    }
    CHECK(text.rfind("the task") > text.find(summary::kBegin));
}

TEST(summary_clean_replaces_git_bash_drive_paths) {
    const std::string cleaned = summary::clean("see /e/repos/x/a.cpp and (/c/Users/bob/y) and `/d/work`");
    CHECK(cleaned.find("repos") == std::string::npos);
    CHECK(cleaned.find("bob") == std::string::npos);
    CHECK_EQ(cleaned, std::string("see <path> and (<path>) and `<path>`"));
}

TEST(summary_extract_refuses_a_huge_block_without_cleaning_it) {
    std::string filler;
    while (filler.size() < 300 * 1024) {
        filler += "/home/bob/x C:/y/z tool output line\n";
    }
    const std::string text = std::string(summary::kBegin) + "\n## Getan\n" + filler +
                             "\n## Commits\n\n## Prüfungen\n\n## Offen\n" + summary::kEnd;
    const auto result = summary::extract(text, "x", "d");
    CHECK(result.summary.is_null());
    CHECK(result.error.find("bytes") != std::string::npos);
    // clean() itself looks at the first kMaxRawBytes only.
    CHECK(summary::clean(filler).size() <= summary::kMaxRawBytes);
}
