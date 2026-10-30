#include <algorithm>
#include <cmath>
#include <filesystem>

#include "config.hpp"
#include "minitest.hpp"
#include "usage.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace usage = ai_desktop::usage;
namespace config = ai_desktop::config;

namespace {

struct Sandbox {
    fs::path dir;
    explicit Sandbox(const std::string& tag) : dir(minitest::make_temp_dir(tag)) {
        minitest::set_env("AI_DESKTOP_CONFIG_DIR", dir.string());
        config::reload();
    }
    ~Sandbox() {
        config::reload();
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }
    fs::path file() const { return dir / "usage.jsonl"; }
};

usage::Event make(const char* ts, const char* provider, const char* model, bool ok, long prompt, long completion) {
    usage::Event e;
    e.ts = ts;
    e.source = "ai-desktop";
    e.provider = provider;
    e.model = model;
    e.ok = ok;
    e.error_kind = ok ? "" : "backend_error";
    e.prompt_tokens = prompt;
    e.completion_tokens = completion;
    e.duration_ms = 42;
    return e;
}

const json kPrices = {
    {"claude-haiku-4-5", {{"input_per_mtok", 1.0}, {"output_per_mtok", 5.0}}},
    {"claude-haiku-4-5-20251001", {{"input_per_mtok", 2.0}, {"output_per_mtok", 10.0}}},
    {"gpt-4o", {{"input_per_mtok", 2.5}, {"output_per_mtok", 10.0}}},
    {"gpt-4o-mini", {{"input_per_mtok", 0.15}, {"output_per_mtok", 0.6}}},
};

}  // namespace

TEST(now_iso8601_has_the_fixed_utc_shape) {
    const std::string ts = usage::now_iso8601();
    CHECK_EQ(ts.size(), static_cast<size_t>(20));  // "YYYY-MM-DDTHH:MM:SSZ"
    CHECK_EQ(ts[4], '-');
    CHECK_EQ(ts[10], 'T');
    CHECK_EQ(ts.back(), 'Z');
}

TEST(append_writes_one_line_per_event_and_read_returns_newest_first) {
    Sandbox box("append");
    usage::Store store(box.file());
    CHECK(store.read_all().empty());  // no file yet: an empty log, no error
    CHECK(store.append(make("2026-10-30T10:00:00Z", "ollama", "llama3.1:8b", true, 10, 20)));
    CHECK(store.append(make("2026-10-30T11:00:00Z", "openai", "gpt-4o", true, 30, 40)));
    CHECK(fs::exists(box.file()));

    const std::string text = minitest::read_file(box.file());
    CHECK_EQ(std::count(text.begin(), text.end(), '\n'), 2);
    // Metadata only: a line carries exactly the contract's fields.
    json first = json::parse(text.substr(0, text.find('\n')));
    CHECK_EQ(first["ts"].get<std::string>(), std::string("2026-10-30T10:00:00Z"));
    CHECK_EQ(first["source"].get<std::string>(), std::string("ai-desktop"));
    CHECK_EQ(first["prompt_tokens"].get<long>(), 10L);
    CHECK(!first.contains("error_kind"));  // omitted when ok
    CHECK(!first.contains("prompt") && !first.contains("text"));

    auto events = store.read_all();
    CHECK_EQ(events.size(), static_cast<size_t>(2));
    CHECK_EQ(events[0].model, std::string("gpt-4o"));  // newest first
    CHECK_EQ(events[1].model, std::string("llama3.1:8b"));
    CHECK_EQ(events[1].duration_ms, 42LL);
}

TEST(for_config_dir_puts_the_log_next_to_config_json) {
    Sandbox box("configdir");
    CHECK_EQ(usage::Store::for_config_dir().path(), box.file());
}

TEST(filters_apply_to_provider_and_an_inclusive_time_range) {
    Sandbox box("filter");
    usage::Store store(box.file());
    store.append(make("2026-10-30T10:00:00Z", "ollama", "llama", true, 1, 1));
    store.append(make("2026-10-30T11:00:00Z", "openai", "gpt-4o", true, 1, 1));
    store.append(make("2026-10-30T12:00:00Z", "openai", "gpt-4o-mini", false, 0, 0));
    store.append(make("2026-10-30T13:00:00Z", "anthropic", "claude-haiku-4-5", true, 1, 1));

    usage::Filter by_provider;
    by_provider.provider = "openai";
    CHECK_EQ(store.read_all(by_provider).size(), static_cast<size_t>(2));

    usage::Filter range;
    range.since = "2026-10-30T11:00:00Z";
    range.until = "2026-10-30T12:00:00Z";
    auto in_range = store.read_all(range);
    CHECK_EQ(in_range.size(), static_cast<size_t>(2));  // both bounds inclusive
    CHECK_EQ(in_range[0].model, std::string("gpt-4o-mini"));

    usage::Filter both;
    both.provider = "openai";
    both.since = "2026-10-30T11:30:00Z";
    CHECK_EQ(store.read_all(both).size(), static_cast<size_t>(1));

    usage::Filter nothing;
    nothing.provider = "gemini";
    CHECK(store.read_all(nothing).empty());
}

TEST(a_broken_line_is_skipped_not_fatal) {
    Sandbox box("broken");
    usage::Store store(box.file());
    store.append(make("2026-10-30T10:00:00Z", "ollama", "llama", true, 1, 1));
    minitest::write_file(box.file(), minitest::read_file(box.file()) + "{ torn line\n\n[1,2,3]\n{\"ts\":\"x\"}\n");
    store.append(make("2026-10-30T11:00:00Z", "ollama", "llama", true, 2, 2));
    auto events = store.read_all();
    CHECK_EQ(events.size(), static_cast<size_t>(2));
    CHECK_EQ(events[0].prompt_tokens, 2LL);
    CHECK_EQ(events[1].prompt_tokens, 1LL);
}

TEST(a_line_with_a_field_of_the_wrong_type_is_read_tolerantly_not_thrown) {
    Sandbox box("wrongtype");
    usage::Store store(box.file());
    store.append(make("2026-10-30T10:00:00Z", "ollama", "llama", true, 1, 1));
    // Hand-edited lines: a null source, a numeric error_kind, a string token count.
    minitest::write_file(box.file(),
                         minitest::read_file(box.file()) +
                             "{\"ts\":\"2026-10-30T10:30:00Z\",\"provider\":\"p\",\"model\":\"m\",\"source\":null}\n"
                             "{\"ts\":\"2026-10-30T10:40:00Z\",\"provider\":\"p\",\"model\":\"m\",\"error_kind\":7,"
                             "\"prompt_tokens\":\"many\",\"ok\":\"yes\"}\n");
    store.append(make("2026-10-30T11:00:00Z", "ollama", "llama", true, 2, 2));
    std::vector<usage::Event> events;
    bool threw = false;
    try {
        events = store.read_all();
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
    CHECK_EQ(events.size(), static_cast<size_t>(4));  // the odd fields read as empty/zero, the line itself is kept
    if (events.size() == 4) {
        CHECK_EQ(events[1].error_kind, std::string(""));
        CHECK_EQ(events[1].prompt_tokens, 0LL);
        CHECK(!events[1].ok);
        CHECK_EQ(events[2].source, std::string(""));
        CHECK_EQ(events[0].prompt_tokens, 2LL);
    }
}

TEST(is_timestamp_accepts_only_the_stored_shape) {
    CHECK(usage::is_timestamp("2026-10-30T20:24:20Z"));
    CHECK(usage::is_timestamp(usage::now_iso8601()));
    CHECK(!usage::is_timestamp(""));
    CHECK(!usage::is_timestamp("2026-10-30"));
    CHECK(!usage::is_timestamp("2026-10-30T20:24:20.000Z"));
    CHECK(!usage::is_timestamp("2026-10-30T20:24:20+00:00"));
    CHECK(!usage::is_timestamp("2026-10-30 20:24:20Z"));
    CHECK(!usage::is_timestamp("2026-1O-30T20:24:20Z"));  // letter O
    CHECK(!usage::is_timestamp("2026-10-30T20:24:20z"));
}

TEST(normalize_name_bounds_a_client_string_like_parse_incoming_does) {
    CHECK_EQ(usage::normalize_name("gpt-4o", usage::kMaxNameLength), std::string("gpt-4o"));
    CHECK_EQ(usage::normalize_name("", usage::kMaxNameLength), std::string(""));
    CHECK_EQ(usage::normalize_name(std::string(5000, 'x'), usage::kMaxNameLength).size(), usage::kMaxNameLength);
    CHECK_EQ(usage::normalize_name(std::string(200, 'x'), usage::kMaxNameLength).size(), static_cast<size_t>(200));
    CHECK_EQ(usage::normalize_name("a\nb", usage::kMaxNameLength), std::string("(invalid)"));
    CHECK_EQ(usage::normalize_name(std::string("a\x7f"), usage::kMaxNameLength), std::string("(invalid)"));
    CHECK_EQ(usage::normalize_name("abcdef", 3), std::string("abc"));
}

TEST(price_lookup_prefers_exact_then_longest_prefix_and_local_is_free) {
    auto exact = usage::price_for("gpt-4o", "openai", kPrices);
    CHECK(exact.found);
    CHECK_EQ(exact.input_per_mtok, 2.5);

    auto mini = usage::price_for("gpt-4o-mini-2024-07-18", "openai", kPrices);
    CHECK(mini.found);
    CHECK_EQ(mini.input_per_mtok, 0.15);  // "gpt-4o-mini" beats the shorter "gpt-4o"

    auto dated = usage::price_for("claude-haiku-4-5-20251001", "anthropic", kPrices);
    CHECK_EQ(dated.input_per_mtok, 2.0);  // exact wins over the prefix "claude-haiku-4-5"

    auto snapshot = usage::price_for("claude-haiku-4-5-20260101", "anthropic", kPrices);
    CHECK_EQ(snapshot.input_per_mtok, 1.0);  // longest prefix

    CHECK(!usage::price_for("claude-opus-9", "anthropic", kPrices).found);
    CHECK(!usage::price_for("gpt-4", "openai", kPrices).found);  // a prefix of a key is not a hit

    auto local = usage::price_for("llama3.1:8b", "ollama", kPrices);
    CHECK(local.found);
    CHECK_EQ(local.input_per_mtok, 0.0);
    CHECK(!usage::price_for("", "openai", json::object()).found);

    // A hand-edited row with non-numeric prices is ignored, never thrown on.
    json broken = kPrices;
    broken["gpt-4o"] = {{"input_per_mtok", "x"}, {"output_per_mtok", 10.0}};
    broken["o3"] = {{"output_per_mtok", 8.0}};
    // Looks a model up without letting a throw abort the whole test binary.
    auto lookup = [&](const char* model, bool& threw) {
        try {
            return usage::price_for(model, "openai", broken);
        } catch (...) {
            threw = true;
            return usage::Price{};
        }
    };
    bool threw = false;
    usage::Price from_broken = lookup("gpt-4o", threw);
    CHECK(!threw);
    CHECK(!from_broken.found);
    CHECK(!lookup("o3", threw).found);
    CHECK_EQ(lookup("gpt-4o-mini", threw).input_per_mtok, 0.15);  // the valid rows still work
    CHECK(!threw);
}

TEST(aggregate_sums_tokens_and_prices_and_nulls_the_cost_on_any_unpriced_event) {
    std::vector<usage::Event> events = {
        make("2026-10-30T10:00:00Z", "openai", "gpt-4o", true, 1000000, 100000),        // 2.5 + 1.0
        make("2026-10-30T11:00:00Z", "openai", "gpt-4o", false, 0, 0),                  // failed, free
        make("2026-10-30T12:00:00Z", "anthropic", "claude-haiku-4-5", true, 2000000, 0),  // 2.0
        make("2026-10-30T13:00:00Z", "ollama", "llama", true, 500, 500),                 // 0
    };
    json agg = usage::aggregate(events, kPrices);
    CHECK_EQ(agg["totals"]["requests"].get<long>(), 4L);
    CHECK_EQ(agg["totals"]["ok"].get<long>(), 3L);
    CHECK_EQ(agg["totals"]["failed"].get<long>(), 1L);
    CHECK_EQ(agg["totals"]["prompt_tokens"].get<long>(), 3000500L);
    CHECK_EQ(agg["totals"]["completion_tokens"].get<long>(), 100500L);
    CHECK_EQ(agg["totals"]["unpriced"].get<long>(), 0L);
    CHECK(agg["totals"]["cost_usd"].is_number());
    CHECK(std::abs(agg["totals"]["cost_usd"].get<double>() - 5.5) < 1e-9);
    CHECK_EQ(agg["by_provider"]["openai"]["requests"].get<long>(), 2L);
    CHECK(std::abs(agg["by_provider"]["openai"]["cost_usd"].get<double>() - 3.5) < 1e-9);
    CHECK_EQ(agg["by_model"]["llama"]["cost_usd"].get<double>(), 0.0);
    CHECK_EQ(agg["by_model"].size(), static_cast<size_t>(3));

    // One unknown model: its groups and the grand total go null, the others keep their sums.
    events.push_back(make("2026-10-30T14:00:00Z", "anthropic", "claude-opus-9", true, 10, 10));
    agg = usage::aggregate(events, kPrices);
    CHECK(agg["totals"]["cost_usd"].is_null());
    CHECK_EQ(agg["totals"]["unpriced"].get<long>(), 1L);
    CHECK(agg["by_provider"]["anthropic"]["cost_usd"].is_null());
    CHECK_EQ(agg["by_provider"]["anthropic"]["unpriced"].get<long>(), 1L);
    CHECK(agg["by_provider"]["openai"]["cost_usd"].is_number());
    CHECK(agg["by_model"]["claude-opus-9"]["cost_usd"].is_null());

    json empty = usage::aggregate({}, kPrices);
    CHECK_EQ(empty["totals"]["requests"].get<long>(), 0L);
    CHECK_EQ(empty["totals"]["cost_usd"].get<double>(), 0.0);
    CHECK(empty["by_provider"].is_object() && empty["by_provider"].empty());
}

TEST(query_limits_the_event_list_but_aggregates_everything) {
    Sandbox box("query");
    usage::Store store(box.file());
    for (int i = 0; i < 5; ++i) {
        store.append(make(("2026-10-30T10:0" + std::to_string(i) + ":00Z").c_str(), "openai", "gpt-4o", true, 1, 1));
    }
    json out = store.query({}, 2, kPrices);
    CHECK_EQ(out["events"].size(), static_cast<size_t>(2));
    CHECK_EQ(out["events"][0]["ts"].get<std::string>(), std::string("2026-10-30T10:04:00Z"));
    CHECK_EQ(out["totals"]["requests"].get<long>(), 5L);
    CHECK_EQ(out["totals"]["prompt_tokens"].get<long>(), 5L);
}

TEST(parse_incoming_accepts_a_minimal_event_and_defaults_the_rest) {
    usage::Event e;
    auto st = usage::parse_incoming(json{{"provider", "anthropic"}, {"model", "claude-haiku-4-5"}}, e);
    CHECK(st.ok());
    CHECK_EQ(e.source, std::string("ai.nvim"));
    CHECK_EQ(e.provider, std::string("anthropic"));
    CHECK(e.ok);
    CHECK(!e.stream);
    CHECK_EQ(e.prompt_tokens, 0LL);
    CHECK_EQ(e.ts.size(), static_cast<size_t>(20));

    st = usage::parse_incoming(json{{"provider", "openai"},
                                    {"model", "gpt-4o"},
                                    {"source", "my-editor"},
                                    {"stream", true},
                                    {"ok", false},
                                    {"error_kind", "timeout"},
                                    {"prompt_tokens", 12},
                                    {"completion_tokens", 0},
                                    {"duration_ms", 1500}},
                               e);
    CHECK(st.ok());
    CHECK_EQ(e.source, std::string("my-editor"));
    CHECK(e.stream && !e.ok);
    CHECK_EQ(e.error_kind, std::string("timeout"));
    CHECK_EQ(e.duration_ms, 1500LL);
}

TEST(parse_incoming_rejects_text_fields_and_bad_values) {
    usage::Event e;
    for (const char* field : {"prompt", "text", "system", "messages", "input", "output", "content", "response"}) {
        json body = {{"provider", "openai"}, {"model", "gpt-4o"}};
        body[field] = "secret";
        auto st = usage::parse_incoming(body, e);
        CHECK_EQ(st.http, 400);
        CHECK(st.message.find(std::string("'") + field + "'") != std::string::npos);  // the message names the field
    }
    CHECK_EQ(usage::parse_incoming(json{{"model", "gpt-4o"}}, e).http, 400);                       // no provider
    CHECK_EQ(usage::parse_incoming(json{{"provider", "openai"}}, e).http, 400);                    // no model
    CHECK_EQ(usage::parse_incoming(json{{"provider", ""}, {"model", "m"}}, e).http, 400);
    CHECK_EQ(usage::parse_incoming(json{{"provider", 5}, {"model", "m"}}, e).http, 400);
    CHECK_EQ(usage::parse_incoming(json{{"provider", "p"}, {"model", "m"}, {"prompt_tokens", -1}}, e).http, 400);
    CHECK_EQ(usage::parse_incoming(json{{"provider", "p"}, {"model", "m"}, {"duration_ms", 1.5}}, e).http, 400);
    CHECK_EQ(usage::parse_incoming(json{{"provider", "p"}, {"model", "m"}, {"ok", "yes"}}, e).http, 400);
    CHECK_EQ(usage::parse_incoming(json{{"provider", "p"}, {"model", "m"}, {"source", std::string(33, 's')}}, e).http,
             400);
    CHECK_EQ(usage::parse_incoming(json{{"provider", "p"}, {"model", std::string("a\nb")}}, e).http, 400);
    CHECK_EQ(usage::parse_incoming(json::array(), e).http, 400);
}

TEST(prices_setting_overlays_the_defaults_and_validates) {
    Sandbox box("prices");
    json defaults = config::default_prices();
    CHECK(defaults.contains("gpt-4o-mini"));
    CHECK_EQ(config::prices(), defaults);

    CHECK(config::update_settings({{"prices", {{"gpt-4o-mini", {{"input_per_mtok", 1.0}, {"output_per_mtok", 2.0}}},
                                              {"my-model", {{"input_per_mtok", 0.0}, {"output_per_mtok", 0.5}}}}}})
              .ok());
    json merged = config::prices();
    CHECK_EQ(merged["gpt-4o-mini"]["input_per_mtok"].get<double>(), 1.0);  // override
    CHECK_EQ(merged["my-model"]["output_per_mtok"].get<double>(), 0.5);    // addition
    CHECK_EQ(merged["gpt-4o"], defaults["gpt-4o"]);                       // untouched default
    config::reload();
    CHECK_EQ(config::prices()["my-model"]["output_per_mtok"].get<double>(), 0.5);  // persisted

    // A single model set to null drops the override; null for the key drops all.
    CHECK(config::update_settings({{"prices", {{"gpt-4o-mini", nullptr}}}}).ok());
    CHECK_EQ(config::prices()["gpt-4o-mini"], defaults["gpt-4o-mini"]);
    CHECK(config::prices().contains("my-model"));
    CHECK(config::update_settings({{"prices", nullptr}}).ok());
    CHECK_EQ(config::prices(), defaults);

    CHECK_EQ(config::update_settings({{"prices", "cheap"}}).http, 400);
    CHECK_EQ(config::update_settings({{"prices", {{"m", {{"input_per_mtok", -1.0}, {"output_per_mtok", 1.0}}}}}}).http,
             400);
    CHECK_EQ(config::update_settings({{"prices", {{"m", {{"input_per_mtok", 1.0}}}}}}).http, 400);
    CHECK_EQ(config::update_settings({{"prices", {{"m", {{"input_per_mtok", "1"}, {"output_per_mtok", 1.0}}}}}}).http,
             400);
    CHECK_EQ(config::update_settings({{"prices", {{"m", {{"input_per_mtok", 1.0}, {"output_per_mtok", 1.0}, {"x", 1}}}}}})
                 .http,
             400);
    CHECK_EQ(config::update_settings({{"prices", {{"", {{"input_per_mtok", 1.0}, {"output_per_mtok", 1.0}}}}}}).http,
             400);
    CHECK_EQ(config::prices(), defaults);  // nothing of the rejected patches stuck
}

TEST(prices_ignores_a_hand_edited_row_that_would_not_pass_put_settings) {
    Sandbox box("handedited");
    json defaults = config::default_prices();
    json doc = config::snapshot();
    doc["settings"]["prices"] = {
        {"gpt-4o", {{"input_per_mtok", "x"}, {"output_per_mtok", 10.0}}},        // string price
        {"o3", {{"input_per_mtok", 1.0}}},                                        // field missing
        {"my-model", {{"input_per_mtok", 0.0}, {"output_per_mtok", 0.5}}},        // valid
    };
    minitest::write_file(box.dir / "config.json", doc.dump());
    config::reload();
    json table = config::prices();
    CHECK_EQ(table["gpt-4o"], defaults["gpt-4o"]);  // the default stays, the broken row is dropped
    CHECK_EQ(table["o3"], defaults["o3"]);
    CHECK_EQ(table["my-model"]["output_per_mtok"].get<double>(), 0.5);
}
