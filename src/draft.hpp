// The draft-task job: turns a free text into the fields of a new task, with its prompt, limits and the check of the
// answer.

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "jobs.hpp"

// A job of the kind `draft-task` (docs/JOBS.md "Drafts"): a free text (a note, a piece of a mail, a thought) becomes
// the fields of a new task. The caller sends the text as `markdown` and the values the vault allows as `choices`; the
// model answers once, without tools, with one JSON object between the markers below. ai-desktop checks it against the
// schema and the choices and keeps the result in the job (`draft`, or `draft_error`); the caller fills its form with
// it, and only the person creates the task.
namespace ai_desktop::draft {

constexpr const char* kKind = "draft-task";
constexpr const char* kBegin = "<!-- draft:begin -->";
constexpr const char* kEnd = "<!-- draft:end -->";
// The schema of the checked draft (the `draft` of a job).
constexpr const char* kSchema = "tasks-draft/1";

// The fixed values of a task (tasks.nvim).
constexpr const char* kEfforts[] = {"XS", "S", "M", "L", "XL"};
constexpr int kMaxPrio = 3;
constexpr int kMaxValue = 5;

// Limits of `choices`: entries per list and bytes per entry.
constexpr std::size_t kMaxChoices = 1000;
constexpr std::size_t kMaxChoiceBytes = 200;
// Limits of a draft.
constexpr std::size_t kMaxTitleBytes = 300;
constexpr std::size_t kMaxSummaryBytes = 2000;
constexpr std::size_t kMaxAcceptance = 20;
constexpr std::size_t kMaxCriterionBytes = 500;
constexpr std::size_t kMaxTags = 20;

// Checks the `choices` of a request: an object of `areas` and `kinds` (each a non-empty list) and, optional,
// `tags`, `categories`, `plans`; every entry [A-Za-z0-9._/-], 1-200 bytes, at most 1000 in a list, no unknown key.
// `out` gets them with every list present (empty when not sent). 400 with a message, or ok.
config::Status parse_choices(const nlohmann::json& value, nlohmann::json& out);

// The prompt: what to do, the allowed values, the answer's form (the markers, the keys), the text as data.
std::string prompt(const jobs::Request& request);

// The system text of the one answer.
std::string system();

// The arguments of `claude` for a draft: print mode, stream-json, no built-in tool (an empty --tools), no MCP server
// and every tool refused besides (one answer, nothing read or written), the model when one is named. The prompt goes on stdin.
std::vector<std::string> claude_args(const std::string& model);

// The tools refused to a draft run (--disallowedTools).
const std::vector<std::string>& disallowed_tools();

// The object of the model checked against the schema and the choices. A value of the wrong type, a missing or empty
// title, or text with a control character make the draft invalid (`error`). A value that is not among the choices
// (an area, a kind, a tag, a category, a plan) or out of range (prio, effort, value) is left out and named in
// `dropped`, so the form keeps its own default there.
struct Result {
    nlohmann::json draft;  // null when invalid
    std::string error;
};
Result check(const nlohmann::json& raw, const nlohmann::json& choices);

// The last block between the markers in the output of a run (a ```json fence inside is allowed), parsed and checked.
Result extract(const std::string& output, const nlohmann::json& choices);

}  // namespace ai_desktop::draft
