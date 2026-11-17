#include <chrono>
#include <filesystem>
#include <string>

#include "config.hpp"
#include "jobs.hpp"
#include "minitest.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace jobs = ai_desktop::jobs;
namespace config = ai_desktop::config;

namespace {

struct Sandbox {
    fs::path dir;
    explicit Sandbox(const std::string& tag) : dir(minitest::make_temp_dir(tag)) {
        minitest::set_env("AI_DESKTOP_CONFIG_DIR", dir.string());
        minitest::set_env("AI_DESKTOP_PRIVATE_AREAS", "");
        config::reload();
        jobs::store().clear();
    }
    ~Sandbox() {
        jobs::store().clear();
        config::reload();
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }
};

json valid_body() {
    return {{"source", "tasks-desktop"},
            {"task_id", "ai-desktop/auftrag-von-aussen"},
            {"title", "Auftrag von außen annehmen"},
            {"markdown", "---\ntitle: x\n---\n\n## Akzeptanz\n\n- [ ] eins\n\tzwei\r\n"},
            {"vault", "E:/repos/tasks-vault"},
            {"area", "ai-desktop"},
            {"mode", "prefill"}};
}

int status_of(const json& body) {
    jobs::Request request;
    return jobs::parse_request(body, request).http;
}

}  // namespace

TEST(jobs_a_valid_request_parses) {
    jobs::Request request;
    const auto st = jobs::parse_request(valid_body(), request);
    CHECK(st.ok());
    CHECK_EQ(request.source, std::string("tasks-desktop"));
    CHECK_EQ(request.area, std::string("ai-desktop"));
    CHECK(request.mode == jobs::Mode::Prefill);
    CHECK_EQ(request.kind, std::string("handoff"));
    CHECK(!request.private_hint);

    json run = valid_body();
    run["mode"] = "run";
    run["model"] = "  claude-haiku-5-5 ";
    run["private"] = true;
    run["kind"] = "split-plan";
    CHECK(jobs::parse_request(run, request).ok());
    CHECK(request.mode == jobs::Mode::Run);
    CHECK_EQ(request.model, std::string("claude-haiku-5-5"));
    CHECK_EQ(request.kind, std::string("split-plan"));
    CHECK(request.private_hint);
}

TEST(jobs_every_kind_is_taken_and_an_unknown_one_refused) {
    for (const char* kind : jobs::kKinds) {
        json body = valid_body();
        body["kind"] = kind;
        CHECK_EQ(status_of(body), 0);
    }
    json body = valid_body();
    body["kind"] = "delete-everything";
    CHECK_EQ(status_of(body), 400);
    body["kind"] = 3;
    CHECK_EQ(status_of(body), 400);
}

TEST(jobs_missing_or_wrong_fields_are_refused) {
    for (const char* field : {"source", "task_id", "title", "markdown", "vault", "area", "mode"}) {
        json body = valid_body();
        body.erase(field);
        CHECK_EQ(status_of(body), 400);
        body[field] = 42;
        CHECK_EQ(status_of(body), 400);
    }
    json body = valid_body();
    body["mode"] = "execute";
    CHECK_EQ(status_of(body), 400);
    body = valid_body();
    body["unexpected"] = "x";
    CHECK_EQ(status_of(body), 400);
    body = valid_body();
    body["private"] = "yes";
    CHECK_EQ(status_of(body), 400);
    CHECK_EQ(status_of(json::array()), 400);
    body = valid_body();
    body["title"] = "   ";
    CHECK_EQ(status_of(body), 400);
}

TEST(jobs_names_and_ids_have_a_small_alphabet) {
    for (const char* id : {"../etc/passwd", "a/../b", "/abs", "a/", "a//b", "a b/c", "a/./b", "ä/b", "."}) {
        json body = valid_body();
        body["task_id"] = id;
        CHECK_EQ(status_of(body), 400);
    }
    for (const char* source : {"Tasks-Desktop", "tasks desktop", "", "a/b"}) {
        json body = valid_body();
        body["source"] = source;
        CHECK_EQ(status_of(body), 400);
    }
    for (const char* area : {"a/b", "..", "a b", ""}) {
        json body = valid_body();
        body["area"] = area;
        CHECK_EQ(status_of(body), 400);
    }
    json body = valid_body();
    body["task_id"] = "tasks.nvim/a_b-c.1";
    CHECK_EQ(status_of(body), 0);
}

TEST(jobs_sizes_are_bounded) {
    json body = valid_body();
    body["markdown"] = std::string(jobs::kMaxMarkdownBytes, 'x');
    CHECK_EQ(status_of(body), 0);
    body["markdown"] = std::string(jobs::kMaxMarkdownBytes + 1, 'x');
    CHECK_EQ(status_of(body), 400);
    body = valid_body();
    body["title"] = std::string(jobs::kMaxTitleBytes + 1, 'x');
    CHECK_EQ(status_of(body), 400);
    body = valid_body();
    body["vault"] = std::string(jobs::kMaxVaultBytes + 1, 'x');
    CHECK_EQ(status_of(body), 400);
}

TEST(jobs_text_follows_the_rules_of_check_control_chars) {
    CHECK_EQ(jobs::text_error("line\nnext\ttab\r\n", true), std::string(""));
    CHECK(!jobs::text_error("line\nnext", false).empty());
    CHECK(!jobs::text_error("tab\there", false).empty());
    CHECK(!jobs::text_error(std::string("nul\0x", 5), true).empty());
    CHECK(!jobs::text_error("esc\x1b[31m", true).empty());
    CHECK(!jobs::text_error("del\x7f", true).empty());
    CHECK(!jobs::text_error("c1 \xc2\x85", true).empty());                  // U+0085
    CHECK(!jobs::text_error("soft\xc2\xad" "hyphen", true).empty());       // U+00AD
    CHECK(!jobs::text_error("zero\xe2\x80\x8bwidth", true).empty());       // U+200B
    CHECK(!jobs::text_error("bidi \xe2\x80\xae override", true).empty());  // U+202E
    CHECK(!jobs::text_error("isolate \xe2\x81\xa6", true).empty());        // U+2066
    CHECK(!jobs::text_error("\xef\xbb\xbf" "bom", true).empty());          // U+FEFF
    CHECK(!jobs::text_error("ls \xe2\x80\xa8", true).empty());             // U+2028
    CHECK(!jobs::text_error("bad \xff utf8", true).empty());
    CHECK(!jobs::text_error("overlong \xc0\xaf", true).empty());
    CHECK(!jobs::text_error("surrogate \xed\xa0\x80", true).empty());
    CHECK(!jobs::text_error("cut \xe2\x82", true).empty());
    CHECK_EQ(jobs::text_error("Umlaute äöü, ein Gedankenstrich – und 🙂", true), std::string(""));
}

TEST(jobs_private_areas_come_from_the_settings_or_the_caller) {
    Sandbox box("jobs-private");
    CHECK(!jobs::is_private("ai-desktop", false));
    CHECK(jobs::is_private("ai-desktop", true));
    CHECK(config::update_settings({{"private_areas", " WKDBook-Tricentis , tricentis-vault,,"}}).ok());
    CHECK_EQ(config::effective("private_areas"), std::string("WKDBook-Tricentis,tricentis-vault"));
    CHECK(jobs::is_private("tricentis-vault", false));
    CHECK(jobs::is_private("WKDBook-Tricentis", false));
    CHECK(!jobs::is_private("wkdbook-tricentis", false));  // area names are exact
    CHECK(!jobs::is_private("ai-desktop", false));
    CHECK_EQ(config::update_settings({{"private_areas", "a b"}}).http, 400);
    CHECK_EQ(config::update_settings({{"private_areas", "../x"}}).http, 400);
    CHECK(config::update_settings({{"private_areas", ""}}).ok());
    CHECK(!jobs::is_private("tricentis-vault", false));

    minitest::set_env("AI_DESKTOP_PRIVATE_AREAS", "secret-area");
    CHECK(jobs::is_private("secret-area", false));
    CHECK_EQ(config::update_settings({{"private_areas", "x"}}).http, 409);
    minitest::set_env("AI_DESKTOP_PRIVATE_AREAS", "");
}

TEST(jobs_a_private_job_goes_to_a_local_model_only) {
    CHECK(jobs::may_send(false, false));
    CHECK(jobs::may_send(false, true));
    CHECK(jobs::may_send(true, true));
    CHECK(!jobs::may_send(true, false));
}

TEST(jobs_the_setting_jobs_is_on_or_off) {
    Sandbox box("jobs-setting");
    CHECK_EQ(config::effective("jobs"), std::string("on"));
    CHECK(config::update_settings({{"jobs", "off"}}).ok());
    CHECK_EQ(config::effective("jobs"), std::string("off"));
    CHECK_EQ(config::update_settings({{"jobs", "maybe"}}).http, 400);
}

TEST(jobs_the_store_runs_holds_and_finishes) {
    Sandbox box("jobs-store");
    jobs::Request request;
    CHECK(jobs::parse_request(valid_body(), request).ok());
    const std::string id = jobs::store().add(request, false);
    CHECK_EQ(id.size(), static_cast<size_t>(16));
    CHECK(jobs::store().get(id)->status == jobs::Status::Prefilled);

    CHECK(jobs::store().hold(id, "gpt-4o").ok());
    CHECK(jobs::store().get(id)->status == jobs::Status::NeedsConfirmation);

    CHECK(jobs::store().start(id, "llama3", std::string("edited")).ok());
    auto job = jobs::store().get(id);
    CHECK(job->status == jobs::Status::Running);
    CHECK_EQ(job->request.markdown, std::string("edited"));
    CHECK_EQ(jobs::store().start(id, "llama3", std::nullopt).http, 409);

    CHECK(jobs::store().append(id, "Hello "));
    CHECK(jobs::store().append(id, "world"));
    jobs::store().finish(id, "");
    job = jobs::store().get(id);
    CHECK(job->status == jobs::Status::Done);
    CHECK_EQ(job->output, std::string("Hello world"));

    const json view = jobs::to_json(*job, false);
    CHECK(!view.contains("markdown"));
    CHECK(!view.contains("output"));
    CHECK_EQ(view["kind"].get<std::string>(), std::string("handoff"));
    CHECK(jobs::to_json(*job, true).contains("markdown"));

    CHECK(jobs::store().start(id, "llama3", std::nullopt).ok());
    jobs::store().finish(id, "backend down");
    CHECK(jobs::store().get(id)->status == jobs::Status::Failed);

    CHECK(jobs::store().remove(id));
    CHECK(!jobs::store().remove(id));
    CHECK(!jobs::store().get(id).has_value());
    CHECK(!jobs::store().append(id, "late"));
    CHECK_EQ(jobs::store().start(id, "x", std::nullopt).http, 404);
}

TEST(jobs_the_store_is_bounded) {
    Sandbox box("jobs-bounded");
    jobs::Request request;
    CHECK(jobs::parse_request(valid_body(), request).ok());
    const std::string first = jobs::store().add(request, false);
    const std::string second = jobs::store().add(request, false);
    CHECK(jobs::store().start(first, "m", std::nullopt).ok());
    CHECK(jobs::store().start(second, "m", std::nullopt).ok());
    const std::string third = jobs::store().add(request, false);
    CHECK_EQ(jobs::store().start(third, "m", std::nullopt).http, 409);  // kMaxRunning
    for (size_t i = 0; i < jobs::kMaxJobs + 5; ++i) {
        jobs::store().add(request, false);
    }
    CHECK_EQ(jobs::store().list().size(), jobs::kMaxJobs);
    CHECK(jobs::store().get(first).has_value());  // running jobs stay
    CHECK(jobs::store().get(second).has_value());
    CHECK(!jobs::store().get(third).has_value());
}

TEST(jobs_the_output_is_cut_at_a_character_boundary) {
    Sandbox box("jobs-output");
    jobs::Request request;
    CHECK(jobs::parse_request(valid_body(), request).ok());
    const std::string id = jobs::store().add(request, false);
    CHECK(jobs::store().start(id, "m", std::nullopt).ok());
    CHECK(jobs::store().append(id, std::string(jobs::kMaxOutputBytes - 1, 'x')));
    CHECK(jobs::store().append(id, "\xc3\xa4\xc3\xa4"));  // "ää": the first does not fit whole
    const auto job = jobs::store().get(id);
    CHECK(job->output_truncated);
    CHECK_EQ(job->output.size(), jobs::kMaxOutputBytes - 1);
    CHECK_EQ(jobs::text_error(job->output, true), std::string(""));
}

TEST(jobs_the_rate_limit_is_a_sliding_window) {
    using namespace std::chrono_literals;
    jobs::RateLimiter limiter(3, 60s);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(limiter.allow(t0));
    CHECK(limiter.allow(t0 + 1s));
    CHECK(limiter.allow(t0 + 2s));
    CHECK(!limiter.allow(t0 + 3s));
    CHECK(limiter.retry_after(t0 + 3s) >= 57);
    CHECK(limiter.allow(t0 + 60s));
    CHECK(!limiter.allow(t0 + 60s));
}

TEST(jobs_ids_are_hex_and_differ) {
    const std::string a = jobs::new_id();
    const std::string b = jobs::new_id();
    CHECK(a != b);
    CHECK_EQ(a.find_first_not_of("0123456789abcdef"), std::string::npos);
}
