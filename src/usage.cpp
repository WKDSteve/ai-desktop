#include "usage.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace ai_desktop::usage {

namespace {

// One mutex for every store: the handlers run in cpp-httplib's worker threads,
// and two of them may finish a request at the same moment.
std::mutex g_write_mutex;

const char* const kForbiddenFields[] = {"prompt", "text",   "system",  "messages",
                                        "input",  "output", "content", "response"};

bool has_control_char(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

// A short identifier-like string field: present, non-empty, bounded, printable.
config::Status read_name(const json& body, const char* key, std::size_t max_length, bool required,
                         std::string& out) {
    if (!body.contains(key)) {
        return required ? config::Status{400, std::string("'") + key + "' (string) is required"} : config::Status{};
    }
    const json& value = body[key];
    if (!value.is_string()) {
        return {400, std::string("'") + key + "' must be a string"};
    }
    out = value.get<std::string>();
    if (required && out.empty()) {
        return {400, std::string("'") + key + "' must not be empty"};
    }
    if (out.size() > max_length) {
        return {400, std::string("'") + key + "' is longer than " + std::to_string(max_length) + " characters"};
    }
    if (has_control_char(out)) {
        return {400, std::string("'") + key + "' must not contain control characters"};
    }
    return {};
}

config::Status read_count(const json& body, const char* key, std::int64_t& out) {
    if (!body.contains(key)) {
        return {};
    }
    const json& value = body[key];
    if (!value.is_number_integer()) {
        return {400, std::string("'") + key + "' must be an integer"};
    }
    const std::int64_t number = value.get<std::int64_t>();
    if (number < 0) {
        return {400, std::string("'") + key + "' must not be negative"};
    }
    out = number;
    return {};
}

config::Status read_flag(const json& body, const char* key, bool& out) {
    if (!body.contains(key)) {
        return {};
    }
    if (!body[key].is_boolean()) {
        return {400, std::string("'") + key + "' must be a boolean"};
    }
    out = body[key].get<bool>();
    return {};
}

std::int64_t count_field(const json& line, const char* key) {
    if (line.contains(key) && line[key].is_number_integer()) {
        const std::int64_t value = line[key].get<std::int64_t>();
        return value < 0 ? 0 : value;
    }
    return 0;
}

// A stored string field; a missing or non-string value (hand-edited file) reads as "".
std::string string_field(const json& line, const char* key) {
    if (line.contains(key) && line[key].is_string()) {
        return line[key].get<std::string>();
    }
    return "";
}

bool is_digits(const std::string& text, std::size_t from, std::size_t count) {
    for (std::size_t i = from; i < from + count; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return true;
}

bool matches(const Event& event, const Filter& filter) {
    if (!filter.provider.empty() && event.provider != filter.provider) {
        return false;
    }
    if (!filter.since.empty() && event.ts < filter.since) {
        return false;
    }
    if (!filter.until.empty() && event.ts > filter.until) {
        return false;
    }
    return true;
}

// Running sums of one aggregation group.
struct Bucket {
    std::int64_t requests = 0;
    std::int64_t ok = 0;
    std::int64_t failed = 0;
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t unpriced = 0;
    double cost_usd = 0.0;

    void add(const Event& event, const Price& price) {
        ++requests;
        if (event.ok) {
            ++ok;
        } else {
            ++failed;
        }
        prompt_tokens += event.prompt_tokens;
        completion_tokens += event.completion_tokens;
        if (price.found) {
            cost_usd += static_cast<double>(event.prompt_tokens) * price.input_per_mtok / 1e6 +
                        static_cast<double>(event.completion_tokens) * price.output_per_mtok / 1e6;
        } else {
            ++unpriced;
        }
    }

    json to_json(bool with_outcome) const {
        json out;
        out["requests"] = requests;
        if (with_outcome) {
            out["ok"] = ok;
            out["failed"] = failed;
        }
        out["prompt_tokens"] = prompt_tokens;
        out["completion_tokens"] = completion_tokens;
        out["cost_usd"] = unpriced > 0 ? json(nullptr) : json(cost_usd);
        out["unpriced"] = unpriced;
        return out;
    }
};

}  // namespace

json Event::to_json() const {
    json out;
    out["ts"] = ts;
    out["source"] = source;
    out["provider"] = provider;
    out["model"] = model;
    out["stream"] = stream;
    out["ok"] = ok;
    if (!error_kind.empty()) {
        out["error_kind"] = error_kind;
    }
    out["prompt_tokens"] = prompt_tokens;
    out["completion_tokens"] = completion_tokens;
    out["duration_ms"] = duration_ms;
    return out;
}

bool Event::from_json(const json& line, Event& out) {
    if (!line.is_object() || !line.contains("ts") || !line["ts"].is_string() || !line.contains("provider") ||
        !line["provider"].is_string() || !line.contains("model") || !line["model"].is_string()) {
        return false;
    }
    out = Event{};
    out.ts = line["ts"].get<std::string>();
    out.provider = line["provider"].get<std::string>();
    out.model = line["model"].get<std::string>();
    out.source = string_field(line, "source");
    out.error_kind = string_field(line, "error_kind");
    out.stream = line.contains("stream") && line["stream"].is_boolean() && line["stream"].get<bool>();
    out.ok = line.contains("ok") && line["ok"].is_boolean() && line["ok"].get<bool>();
    out.prompt_tokens = count_field(line, "prompt_tokens");
    out.completion_tokens = count_field(line, "completion_tokens");
    out.duration_ms = count_field(line, "duration_ms");
    return true;
}

std::string now_iso8601() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buffer;
}

bool is_timestamp(const std::string& text) {
    // "YYYY-MM-DDTHH:MM:SSZ", the exact shape now_iso8601() produces.
    return text.size() == 20 && is_digits(text, 0, 4) && text[4] == '-' && is_digits(text, 5, 2) && text[7] == '-' &&
           is_digits(text, 8, 2) && text[10] == 'T' && is_digits(text, 11, 2) && text[13] == ':' &&
           is_digits(text, 14, 2) && text[16] == ':' && is_digits(text, 17, 2) && text[19] == 'Z';
}

std::string normalize_name(std::string value, std::size_t max_length) {
    if (has_control_char(value)) {
        return "(invalid)";
    }
    if (value.size() > max_length) {
        value.resize(max_length);
    }
    return value;
}

bool is_forbidden_field(const std::string& key) {
    for (const char* name : kForbiddenFields) {
        if (key == name) {
            return true;
        }
    }
    return false;
}

config::Status parse_incoming(const json& body, Event& out) {
    if (!body.is_object()) {
        return {400, "the request body must be a JSON object"};
    }
    // Privacy first: refuse before looking at anything else, so a body that
    // carries conversation text is never half-accepted.
    for (const auto& item : body.items()) {
        if (is_forbidden_field(item.key())) {
            return {400, "field '" + item.key() + "' is not allowed: usage events carry metadata only, never text"};
        }
    }

    Event event;
    config::Status st;
    if (!(st = read_name(body, "provider", kMaxNameLength, true, event.provider)).ok()) return st;
    if (!(st = read_name(body, "model", kMaxNameLength, true, event.model)).ok()) return st;
    if (!(st = read_name(body, "error_kind", kMaxErrorKindLength, false, event.error_kind)).ok()) return st;
    if (!(st = read_name(body, "source", kMaxSourceLength, false, event.source)).ok()) return st;
    if (!(st = read_flag(body, "stream", event.stream)).ok()) return st;
    event.ok = true;
    if (!(st = read_flag(body, "ok", event.ok)).ok()) return st;
    if (!(st = read_count(body, "prompt_tokens", event.prompt_tokens)).ok()) return st;
    if (!(st = read_count(body, "completion_tokens", event.completion_tokens)).ok()) return st;
    if (!(st = read_count(body, "duration_ms", event.duration_ms)).ok()) return st;

    if (event.source.empty()) {
        event.source = "ai.nvim";
    }
    event.ts = now_iso8601();
    out = std::move(event);
    return {};
}

Price price_for(const std::string& model, const std::string& provider, const json& prices) {
    Price best;
    std::size_t best_length = 0;
    if (prices.is_object()) {
        for (const auto& item : prices.items()) {
            const std::string& key = item.key();
            const json& entry = item.value();
            if (!entry.is_object() || key.empty() || !entry.contains("input_per_mtok") ||
                !entry["input_per_mtok"].is_number() || !entry.contains("output_per_mtok") ||
                !entry["output_per_mtok"].is_number()) {
                continue;  // not a usable row (a hand-edited config.json)
            }
            const bool exact = key == model;
            const bool prefix = model.size() > key.size() && model.compare(0, key.size(), key) == 0;
            if (!exact && !prefix) {
                continue;
            }
            // An exact hit always wins; among prefixes the longest one does.
            const std::size_t length = exact ? model.size() + 1 : key.size();
            if (best.found && length <= best_length) {
                continue;
            }
            best.found = true;
            best_length = length;
            best.input_per_mtok = entry["input_per_mtok"].get<double>();
            best.output_per_mtok = entry["output_per_mtok"].get<double>();
        }
    }
    if (!best.found && provider == "ollama") {
        best.found = true;  // a local model costs nothing
    }
    return best;
}

json aggregate(const std::vector<Event>& events, const json& prices) {
    Bucket totals;
    std::map<std::string, Bucket> by_provider;
    std::map<std::string, Bucket> by_model;
    for (const Event& event : events) {
        const Price price = price_for(event.model, event.provider, prices);
        totals.add(event, price);
        by_provider[event.provider].add(event, price);
        by_model[event.model].add(event, price);
    }

    json out;
    out["totals"] = totals.to_json(true);
    out["by_provider"] = json::object();
    for (const auto& [name, bucket] : by_provider) {
        out["by_provider"][name] = bucket.to_json(false);
    }
    out["by_model"] = json::object();
    for (const auto& [name, bucket] : by_model) {
        out["by_model"][name] = bucket.to_json(false);
    }
    return out;
}

Store::Store(fs::path file) : file_(std::move(file)) {}

Store Store::for_config_dir() {
    return Store(fs::u8path(config::directory()) / "usage.jsonl");
}

bool Store::append(const Event& event, std::string* error) const {
    try {
        const std::string line = event.to_json().dump() + "\n";
        std::lock_guard<std::mutex> lock(g_write_mutex);
        std::error_code ec;
        fs::create_directories(file_.parent_path(), ec);
        if (ec) {
            if (error) *error = "cannot create " + file_.parent_path().u8string() + ": " + ec.message();
            return false;
        }
        std::ofstream out(file_, std::ios::binary | std::ios::app);
        if (!out) {
            if (error) *error = "cannot open " + file_.u8string();
            return false;
        }
        out << line;
        out.flush();
        if (!out) {
            if (error) *error = "failed while writing " + file_.u8string();
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

std::vector<Event> Store::read_all(const Filter& filter) const {
    std::vector<Event> events;
    std::ifstream in(file_, std::ios::binary);
    if (!in) {
        return events;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        // A torn or foreign line must not take the whole log down: neither a
        // syntax error nor a field of the wrong type (the parse is non-throwing,
        // the catch is a belt for anything from_json might still throw).
        try {
            json parsed = json::parse(line, nullptr, false);
            Event event;
            if (parsed.is_discarded() || !Event::from_json(parsed, event)) {
                continue;
            }
            if (matches(event, filter)) {
                events.push_back(std::move(event));
            }
        } catch (const std::exception&) {
            continue;
        }
    }
    std::reverse(events.begin(), events.end());
    return events;
}

json Store::query(const Filter& filter, std::size_t limit, const json& prices) const {
    const std::vector<Event> events = read_all(filter);
    json out = aggregate(events, prices);
    json list = json::array();
    for (std::size_t i = 0; i < events.size() && i < limit; ++i) {
        list.push_back(events[i].to_json());
    }
    out["events"] = std::move(list);
    return out;
}

}  // namespace ai_desktop::usage
