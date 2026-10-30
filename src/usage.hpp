#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "config.hpp"
#include "nlohmann/json.hpp"

// Usage log: one JSON line per request in <config dir>/usage.jsonl, shared by
// this server (its own /ask and /ask/stream requests) and ai.nvim (POST
// /usage/events). Metadata only -- a line never holds prompt, system or answer
// text, and the API refuses an event that carries a field of that kind.
//
// The file is opened, appended to and closed for every event (no handle stays
// open), writes are serialised with a mutex, and a line that does not parse or
// has a field of the wrong type (a hand-edited file) is skipped when reading
// rather than failing the whole query. Pure logic, no HTTP, so it lives in
// ai_desktop_core and is unit-tested directly.
namespace ai_desktop::usage {

// Bounds of the identifier-like string fields, enforced on POST /usage/events
// and, through normalize_name(), on the events the server records itself.
constexpr std::size_t kMaxNameLength = 200;       // provider, model
constexpr std::size_t kMaxSourceLength = 32;      // source
constexpr std::size_t kMaxErrorKindLength = 64;   // error_kind

struct Event {
    std::string ts;          // ISO 8601 UTC, "2026-10-30T12:34:56Z"
    std::string source;      // "ai-desktop" | "ai.nvim" | another client (<= 32 chars)
    std::string provider;    // backend the ModelRouter picked (ollama/openai/anthropic/gemini/openai-compatible)
    std::string model;       // model name as requested
    bool stream = false;
    bool ok = false;
    std::string error_kind;  // "" when ok; e.g. backend_error, invalid_request, no_key
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t duration_ms = 0;

    nlohmann::json to_json() const;
    // Fills `out` from a stored line; false when the line is not a usable event.
    static bool from_json(const nlohmann::json& line, Event& out);
};

// Query filter: an empty field means "no restriction". since/until compare
// inclusively against `ts` as strings, which is exact only for the fixed-width
// UTC format every stored line uses -- the API accepts nothing else, see
// is_timestamp().
struct Filter {
    std::string provider;
    std::string since;
    std::string until;
};

// Current time as the ISO 8601 UTC string stored in `ts`.
std::string now_iso8601();

// True for exactly the stored timestamp shape "YYYY-MM-DDTHH:MM:SSZ" (digits in
// the right places, no fraction, no offset). Other ISO 8601 spellings would
// compare wrongly against `ts`, so the API refuses them.
bool is_timestamp(const std::string& text);

// Makes a string from a request safe to store as an identifier-like field: a
// value with a control character becomes "(invalid)", a longer one is cut to
// `max_length`. The server's own events pass the client's model name through
// this, so they obey the same bounds POST /usage/events enforces.
std::string normalize_name(std::string value, std::size_t max_length);

// Field names a usage event must never carry (they would hold conversation
// text). The check is on the top-level keys of a POST /usage/events body.
bool is_forbidden_field(const std::string& key);

// Validates a POST /usage/events body and turns it into an event with `ts` set
// to now and `source` defaulting to "ai.nvim". 400 with a message naming the
// problem (and, for a privacy violation, the offending field) otherwise.
config::Status parse_incoming(const nlohmann::json& body, Event& out);

// Price of one model in USD per million tokens. `found` is false when the
// table has no entry for the model (and the provider is not a local one).
struct Price {
    double input_per_mtok = 0.0;
    double output_per_mtok = 0.0;
    bool found = false;
};

// Looks `model` up in a price table {"<model>": {"input_per_mtok", "output_per_mtok"}}:
// an exact key first, else the longest key that is a prefix of the model name.
// A local provider ("ollama") costs nothing when the table has no entry. An
// entry whose prices are not numbers (hand-edited config) is ignored.
Price price_for(const std::string& model, const std::string& provider, const nlohmann::json& prices);

// Totals over `events`:
//   {"totals": {"requests","ok","failed","prompt_tokens","completion_tokens","cost_usd","unpriced"},
//    "by_provider": {"<provider>": {"requests","prompt_tokens","completion_tokens","cost_usd","unpriced"}},
//    "by_model": {...same shape...}}
// cost_usd is null as soon as one event in that group has no price (no silent
// partial sums); `unpriced` counts those events.
nlohmann::json aggregate(const std::vector<Event>& events, const nlohmann::json& prices);

class Store {
public:
    explicit Store(std::filesystem::path file);

    // The store of the running server: usage.jsonl next to config.json.
    static Store for_config_dir();

    const std::filesystem::path& path() const { return file_; }

    // Appends one line; false (with `error` filled) when the file cannot be
    // written. Never throws.
    bool append(const Event& event, std::string* error = nullptr) const;

    // Every stored event that matches `filter`, newest first. A line that does
    // not parse or does not describe an event is skipped. A missing file is an
    // empty log. Never throws.
    std::vector<Event> read_all(const Filter& filter = {}) const;

    // The GET /usage response: the newest `limit` matching events plus the
    // aggregates over *all* matching events.
    nlohmann::json query(const Filter& filter, std::size_t limit, const nlohmann::json& prices) const;

private:
    std::filesystem::path file_;
};

}  // namespace ai_desktop::usage
