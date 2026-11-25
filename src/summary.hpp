// The summary block at the end of a hand-over run: finding it in the output, cleaning it and checking its form and
// size.

#pragma once

#include <string>

#include <nlohmann/json.hpp>

// The summary at the end of a `handoff` run (docs/JOBS.md "Summary of a hand-over"; decision 2026-11-23, option a):
// the agent ends its last message with one block between the markers below (what was done, the commits, the checks
// with their verdict, what is open). ai-desktop extracts it, cleans it (the character rule of the jobs, no absolute
// path), checks its form and size, and keeps it in the job (`summary`, or `summary_error`). Nothing is written to a
// disk: the caller (tasks-desktop, tasks.nvim) shows it like a review answer and adds it to the task as a section
// only after a yes.
namespace ai_desktop::summary {

constexpr const char* kBegin = "<!-- summary:begin -->";
constexpr const char* kEnd = "<!-- summary:end -->";
// The schema of the kept summary (the `summary` of a job).
constexpr const char* kSchema = "handoff-summary/1";
// What the block may hold at most, after cleaning ("about 4 KiB").
constexpr std::size_t kMaxBytes = 4096;
// What clean() looks at at most, and the raw block extract() takes at most (cleaning only shortens, and std::regex
// is recursive or quadratic on long text: the output of a run can be 512 KiB).
constexpr std::size_t kMaxRawBytes = 2 * kMaxBytes;

// The headings the block must have, in this order.
constexpr const char* kHeadings[] = {"## Getan", "## Commits", "## Prüfungen", "## Offen"};

struct Result {
    nlohmann::json summary;  // {schema, job_id, date, markdown}; null when there is none
    std::string error;       // why there is none
};

// The text of the last block in `output`, cleaned: foreign text, so every byte the character rule refuses becomes
// U+FFFD, and an absolute path (`C:\...`, `\\host\...`, `/home/...`) becomes `<path>`. Empty when there is no block.
std::string clean(const std::string& block);

// Takes the last block of the agent's output, cleans it and checks it: all headings of kHeadings, in order, and at
// most kMaxBytes. `job_id` and `date` (YYYY-MM-DD, UTC) go into the result.
Result extract(const std::string& output, const std::string& job_id, const std::string& date);

// The part of the prompt that asks for the block.
std::string instructions();

}  // namespace ai_desktop::summary
