#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>

#include "draft.hpp"
#include "jobs.hpp"
#include "minitest.hpp"

namespace draft = ai_desktop::draft;
namespace jobs = ai_desktop::jobs;
using json = nlohmann::json;

namespace {

json choices() {
    json out;
    const json given = {{"areas", {"ai-desktop", "tasks.nvim"}},
                        {"kinds", {"task", "feature", "bug"}},
                        {"tags", {"ui", "tasks-family"}},
                        {"categories", {"ux"}},
                        {"plans", {"ALL/ki-plan"}}};
    const auto st = draft::parse_choices(given, out);
    return st.ok() ? out : json();
}

json request_body(json extra) {
    json body = {{"source", "tasks-desktop"}, {"task_id", "draft/new"}, {"title", "Neue Task"},
                 {"markdown", "Der Knopf soll blau sein."}, {"vault", "E:/repos/tasks-vault"}, {"area", ""},
                 {"mode", "run"}, {"kind", "draft-task"}, {"choices", choices()}};
    for (auto& item : extra.items()) {
        if (item.value().is_null()) {
            body.erase(item.key());
        } else {
            body[item.key()] = item.value();
        }
    }
    return body;
}

}  // namespace

TEST(draft_choices_need_areas_and_kinds_and_plain_names) {
    json out;
    CHECK(draft::parse_choices(json{{"areas", {"a"}}, {"kinds", {"task"}}}, out).ok());
    CHECK_EQ(out["tags"].size(), std::size_t{0});
    CHECK_EQ(out["plans"].size(), std::size_t{0});
    CHECK(!draft::parse_choices(json{{"areas", json::array()}, {"kinds", {"task"}}}, out).ok());
    CHECK(!draft::parse_choices(json{{"areas", {"a"}}}, out).ok());
    CHECK(!draft::parse_choices(json{{"areas", {"a b"}}, {"kinds", {"task"}}}, out).ok());
    CHECK(!draft::parse_choices(json{{"areas", {".."}}, {"kinds", {"task"}}}, out).ok());
    CHECK(!draft::parse_choices(json{{"areas", {"a"}}, {"kinds", {"task"}}, {"people", {"x"}}}, out).ok());
    CHECK(!draft::parse_choices(json{{"areas", {1}}, {"kinds", {"task"}}}, out).ok());
    // Doubles are kept once.
    CHECK(draft::parse_choices(json{{"areas", {"a", "a"}}, {"kinds", {"task"}}}, out).ok());
    CHECK_EQ(out["areas"].size(), std::size_t{1});
}

TEST(draft_request_may_leave_the_area_empty_and_needs_choices) {
    jobs::Request r;
    CHECK(jobs::parse_request(request_body(json::object()), r).ok());
    CHECK_EQ(r.kind, std::string("draft-task"));
    CHECK(r.area.empty());
    CHECK(r.choices.is_object());
    CHECK(!jobs::parse_request(request_body(json{{"choices", nullptr}}), r).ok());
    // Another kind keeps its area required and takes no choices.
    CHECK(!jobs::parse_request(request_body(json{{"kind", "handoff"}, {"choices", nullptr}}), r).ok());
    CHECK(!jobs::parse_request(request_body(json{{"kind", "handoff"}, {"area", "x"}}), r).ok());
    CHECK(jobs::parse_request(request_body(json{{"kind", "handoff"}, {"area", "x"}, {"choices", nullptr}}), r).ok());
}

TEST(draft_prompt_lists_the_choices_and_holds_the_text_as_data) {
    jobs::Request r;
    CHECK(jobs::parse_request(request_body(json::object()), r).ok());
    const std::string p = draft::prompt(r);
    CHECK(p.find("ai-desktop, tasks.nvim") != std::string::npos);
    CHECK(p.find("ALL/ki-plan") != std::string::npos);
    CHECK(p.find(draft::kBegin) != std::string::npos);
    CHECK(p.find("~~~~text\nDer Knopf soll blau sein.\n~~~~") != std::string::npos);
}

TEST(draft_claude_refuses_every_tool) {
    const auto args = draft::claude_args("sonnet");
    CHECK(std::find(args.begin(), args.end(), "--allowedTools") == args.end());
    CHECK(std::find(args.begin(), args.end(), "--add-dir") == args.end());
    for (const char* tool : {"Bash", "Read", "Write", "WebFetch"}) {
        CHECK(std::find(args.begin(), args.end(), tool) != args.end());
    }
    CHECK(std::find(args.begin(), args.end(), "sonnet") != args.end());
}

TEST(draft_check_keeps_allowed_values_and_drops_others) {
    const json raw = {{"title", "  Knopf blau machen  "},
                      {"area", "tasks.nvim"},
                      {"kind", "chore"},
                      {"prio", "2"},
                      {"effort", "XXL"},
                      {"value", 9},
                      {"tags", {"ui", "made-up", "ui"}},
                      {"category", "ux"},
                      {"plan", ""},
                      {"summary", "Zeile eins.\nZeile zwei."},
                      {"acceptance", {"- [ ] Der Knopf ist blau", "", "Kontrast reicht"}}};
    const auto result = draft::check(raw, choices());
    CHECK(result.error.empty());
    const json& d = result.draft;
    CHECK_EQ(d["schema"].get<std::string>(), std::string(draft::kSchema));
    CHECK_EQ(d["title"].get<std::string>(), std::string("Knopf blau machen"));
    CHECK_EQ(d["area"].get<std::string>(), std::string("tasks.nvim"));
    CHECK(!d.contains("kind"));
    CHECK(!d.contains("plan"));
    CHECK(!d.contains("effort"));
    CHECK(!d.contains("value"));
    CHECK_EQ(d["prio"].get<int>(), 2);
    CHECK_EQ(d["tags"], json({"ui"}));
    CHECK_EQ(d["category"], json({"ux"}));
    CHECK_EQ(d["acceptance"], json({"Der Knopf ist blau", "Kontrast reicht"}));
    const std::string dropped = d["dropped"].dump();
    for (const char* word : {"kind: chore", "tags: made-up", "effort: XXL", "value: 9"}) {
        CHECK(dropped.find(word) != std::string::npos);
    }
}

TEST(draft_check_refuses_a_wrong_shape) {
    CHECK(!draft::check(json::array(), choices()).error.empty());
    CHECK(!draft::check(json{{"kind", "task"}}, choices()).error.empty());
    CHECK(!draft::check(json{{"title", ""}}, choices()).error.empty());
    CHECK(!draft::check(json{{"title", "a\nb"}}, choices()).error.empty());
    CHECK(!draft::check(json{{"title", "x"}, {"prio", "high"}}, choices()).error.empty());
    CHECK(!draft::check(json{{"title", "x"}, {"tags", 3}}, choices()).error.empty());
    CHECK(!draft::check(json{{"title", "x"}, {"acceptance", "one"}}, choices()).error.empty());
    CHECK(!draft::check(json{{"title", "x"}, {"summary", "a\x1b[31m"}}, choices()).error.empty());
    CHECK(!draft::check(json{{"title", std::string(400, 'a')}}, choices()).error.empty());
    CHECK(draft::check(json{{"title", "x"}}, choices()).error.empty());
}

TEST(draft_extract_takes_the_last_block_with_or_without_a_fence) {
    const std::string plain = std::string("Thinking.\n") + draft::kBegin + "\n{\"title\": \"A\"}\n" + draft::kEnd;
    CHECK_EQ(draft::extract(plain, choices()).draft["title"].get<std::string>(), std::string("A"));
    const std::string fenced = std::string(draft::kBegin) + "\n{\"title\": \"old\"}\n" + draft::kEnd + "\n" +
                               draft::kBegin + "\n```json\n{\"title\": \"B\", \"kind\": \"bug\"}\n```\n" + draft::kEnd;
    const auto b = draft::extract(fenced, choices());
    CHECK_EQ(b.draft["title"].get<std::string>(), std::string("B"));
    CHECK_EQ(b.draft["kind"].get<std::string>(), std::string("bug"));
    CHECK(!draft::extract("no block", choices()).error.empty());
    CHECK(!draft::extract(std::string(draft::kBegin) + "{\"title\": \"x\"}", choices()).error.empty());
    CHECK(!draft::extract(std::string(draft::kBegin) + "{title: x}" + draft::kEnd, choices()).error.empty());
}
