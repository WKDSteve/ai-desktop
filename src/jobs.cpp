#include "jobs.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <random>
#include <string_view>

#include "draft.hpp"
#include "usage.hpp"

using json = nlohmann::json;

namespace ai_desktop::jobs {

namespace {

// Code points refused in any text (tools/check-control-chars.py, the same ranges); line feed, carriage return and
// tab are let through by text_error() for a multi-line text before this table is asked.
constexpr std::pair<std::uint32_t, std::uint32_t> kRefused[] = {
    {0x00, 0x1F},     {0x7F, 0x9F},     {0xAD, 0xAD},     {0x200B, 0x200F}, {0x2028, 0x2029},
    {0x202A, 0x202E}, {0x2060, 0x2064}, {0x2066, 0x2069}, {0xFEFF, 0xFEFF},
};

bool refused(std::uint32_t cp) {
    for (const auto& [low, high] : kRefused) {
        if (cp >= low && cp <= high) {
            return true;
        }
    }
    return false;
}

// Decodes one UTF-8 sequence at `i`; false for an invalid one (overlong, surrogate, beyond U+10FFFF, cut short).
bool decode(const std::string& text, std::size_t& i, std::uint32_t& cp) {
    const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
    const unsigned char lead = byte(i);
    std::size_t length = 0;
    std::uint32_t min = 0;
    if (lead < 0x80) {
        cp = lead;
        i += 1;
        return true;
    } else if ((lead & 0xE0) == 0xC0) {
        length = 2;
        cp = lead & 0x1F;
        min = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        cp = lead & 0x0F;
        min = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        cp = lead & 0x07;
        min = 0x10000;
    } else {
        return false;
    }
    if (i + length > text.size()) {
        return false;
    }
    for (std::size_t k = 1; k < length; ++k) {
        const unsigned char next = byte(i + k);
        if ((next & 0xC0) != 0x80) {
            return false;
        }
        cp = (cp << 6) | (next & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return false;
    }
    i += length;
    return true;
}

// Only the characters of `extra` besides ASCII letters and digits.
bool plain(const std::string& text, const char* extra, bool lower_only) {
    for (unsigned char c : text) {
        const bool letter = (c >= 'a' && c <= 'z') || (!lower_only && c >= 'A' && c <= 'Z');
        const bool digit = c >= '0' && c <= '9';
        if (!letter && !digit && std::string_view(extra).find(static_cast<char>(c)) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

bool valid_task_id(const std::string& id) {
    if (id.empty() || id.size() > kMaxTaskIdBytes || !plain(id, "._/-", false)) {
        return false;
    }
    if (id.front() == '/' || id.back() == '/' || id.find("//") != std::string::npos) {
        return false;
    }
    // No segment may be "." or ".." (the id names a file below the vault on the side of the caller).
    std::size_t start = 0;
    while (start <= id.size()) {
        const std::size_t end = std::min(id.find('/', start), id.size());
        const std::string segment = id.substr(start, end - start);
        if (segment == "." || segment == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

bool valid_area(const std::string& area) {
    return !area.empty() && area.size() <= kMaxAreaBytes && plain(area, "._-", false) && area != "." && area != "..";
}

std::string trimmed(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return "";
    }
    return text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
}

// A required string field, bounded, one line or more.
config::Status string_field(const json& body, const char* name, std::size_t max, bool multiline, bool required,
                            std::string& out) {
    if (!body.contains(name) || body[name].is_null()) {
        if (required) {
            return {400, std::string("'") + name + "' (string) is required"};
        }
        out.clear();
        return {};
    }
    if (!body[name].is_string()) {
        return {400, std::string("'") + name + "' must be a string"};
    }
    out = body[name].get<std::string>();
    if (out.size() > max) {
        return {400, std::string("'") + name + "' is longer than " + std::to_string(max) + " bytes"};
    }
    if (const std::string error = text_error(out, multiline); !error.empty()) {
        return {400, std::string("'") + name + "': " + error};
    }
    return {};
}

}  // namespace

std::string text_error(const std::string& text, bool multiline) {
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t at = i;
        std::uint32_t cp = 0;
        if (!decode(text, i, cp)) {
            return "not valid UTF-8 (byte " + std::to_string(at) + ")";
        }
        if (multiline && (cp == '\n' || cp == '\r' || cp == '\t')) {
            continue;
        }
        if (refused(cp)) {
            char hex[16];
            std::snprintf(hex, sizeof hex, "U+%04X", static_cast<unsigned>(cp));
            return std::string("holds the control or invisible character ") + hex + " (byte " + std::to_string(at) +
                   ")";
        }
    }
    return "";
}

std::string sanitize(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t at = i;
        std::uint32_t cp = 0;
        if (!decode(text, i, cp)) {
            out += "\xEF\xBF\xBD";
            i = at + 1;
            continue;
        }
        if ((cp == '\n' || cp == '\r' || cp == '\t') || !refused(cp)) {
            out.append(text, at, i - at);
        } else {
            out += "\xEF\xBF\xBD";
        }
    }
    return out;
}

config::Status parse_request(const json& body, Request& out) {
    if (!body.is_object()) {
        return {400, "the request body must be a JSON object"};
    }
    static const char* const kKnown[] = {"source", "task_id", "title", "markdown", "vault",
                                         "area",   "mode",    "kind",  "model",    "private",
                                         "choices"};
    for (const auto& item : body.items()) {
        if (std::find_if(std::begin(kKnown), std::end(kKnown), [&](const char* k) { return item.key() == k; }) ==
            std::end(kKnown)) {
            return {400, "unknown field '" + item.key() + "'"};
        }
    }
    Request r;
    config::Status st;
    if (!(st = string_field(body, "source", kMaxSourceBytes, false, true, r.source)).ok()) return st;
    if (r.source.empty() || !plain(r.source, "._-", true)) {
        return {400, "'source': expected the name of an app, [a-z0-9._-]"};
    }
    if (!(st = string_field(body, "task_id", kMaxTaskIdBytes, false, true, r.task_id)).ok()) return st;
    if (!valid_task_id(r.task_id)) {
        return {400, "'task_id': expected <area>/<slug> of [A-Za-z0-9._-], no '.' or '..' segment"};
    }
    if (!(st = string_field(body, "title", kMaxTitleBytes, false, true, r.title)).ok()) return st;
    if (trimmed(r.title).empty()) {
        return {400, "'title' must not be empty"};
    }
    if (!(st = string_field(body, "markdown", kMaxMarkdownBytes, true, true, r.markdown)).ok()) return st;
    if (!(st = string_field(body, "vault", kMaxVaultBytes, false, true, r.vault)).ok()) return st;
    if (!(st = string_field(body, "area", kMaxAreaBytes, false, true, r.area)).ok()) return st;
    std::string mode;
    if (!(st = string_field(body, "mode", 16, false, true, mode)).ok()) return st;
    if (mode == "prefill") {
        r.mode = Mode::Prefill;
    } else if (mode == "run") {
        r.mode = Mode::Run;
    } else {
        return {400, "'mode': expected prefill or run"};
    }
    if (!(st = string_field(body, "kind", 32, false, false, r.kind)).ok()) return st;
    if (r.kind.empty()) {
        r.kind = "handoff";
    } else if (!valid_kind(r.kind)) {
        return {400, "'kind': expected handoff, review-done, draft-task, prepare-decision or split-plan"};
    }
    // A draft is made before its area is known: there the area may be empty (and the job counts as private).
    const bool is_draft = r.kind == draft::kKind;
    if (!valid_area(r.area) && !(is_draft && r.area.empty())) {
        return {400, "'area': expected [A-Za-z0-9._-]"};
    }
    if (is_draft) {
        if (!body.contains("choices")) {
            return {400, "'choices' is required for a draft-task"};
        }
        if (!(st = draft::parse_choices(body["choices"], r.choices)).ok()) return st;
    } else if (body.contains("choices")) {
        return {400, "'choices' belongs to a draft-task only"};
    }
    if (!(st = string_field(body, "model", kMaxModelBytes, false, false, r.model)).ok()) return st;
    r.model = trimmed(r.model);
    if (body.contains("private") && !body["private"].is_null()) {
        if (!body["private"].is_boolean()) {
            return {400, "'private' must be true or false"};
        }
        r.private_hint = body["private"].get<bool>();
    }
    out = std::move(r);
    return {};
}

bool valid_kind(const std::string& kind) {
    return std::find_if(std::begin(kKinds), std::end(kKinds), [&](const char* k) { return kind == k; }) !=
           std::end(kKinds);
}

std::vector<std::string> private_areas() {
    std::vector<std::string> out;
    const std::string value = config::effective("private_areas");
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = std::min(value.find(',', start), value.size());
        const std::string name = trimmed(value.substr(start, end - start));
        if (!name.empty()) {
            out.push_back(name);
        }
        start = end + 1;
    }
    return out;
}

config::Status normalize_private_areas(std::string& value) {
    std::string normalized;
    std::size_t count = 0;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = std::min(value.find(',', start), value.size());
        const std::string name = trimmed(value.substr(start, end - start));
        start = end + 1;
        if (name.empty()) {
            continue;
        }
        if (!valid_area(name)) {
            return {400, "private_areas: '" + name + "' is not an area name ([A-Za-z0-9._-], up to 100 characters)"};
        }
        if (++count > 200) {
            return {400, "private_areas: more than 200 areas"};
        }
        normalized += (normalized.empty() ? "" : ",") + name;
    }
    value = normalized;
    return {};
}

bool is_private(const std::string& area, bool private_hint) {
    if (private_hint) {
        return true;
    }
    // Without regard to case: an area is a folder of the vault, and on Windows and macOS "Kunde" and "kunde" are the
    // same one. A name in another case counts as private rather than going to a cloud model unasked. Area names are
    // ASCII ([A-Za-z0-9._-], valid_area), so folding ASCII letters is enough.
    const auto fold = [](unsigned char c) { return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + 32) : c; };
    const auto areas = private_areas();
    return std::any_of(areas.begin(), areas.end(), [&](const std::string& name) {
        return name.size() == area.size() &&
               std::equal(name.begin(), name.end(), area.begin(), [&](char a, char b) {
                   return fold(static_cast<unsigned char>(a)) == fold(static_cast<unsigned char>(b));
               });
    });
}

bool may_send(bool is_private_job, bool model_is_local) {
    return !is_private_job || model_is_local;
}

const char* status_name(Status status) {
    switch (status) {
        case Status::Prefilled: return "prefilled";
        case Status::NeedsConfirmation: return "needs_confirmation";
        case Status::Running: return "running";
        case Status::Done: return "done";
        case Status::Failed: return "failed";
        case Status::Cancelled: return "cancelled";
    }
    return "unknown";
}

json to_json(const Job& job, bool with_text) {
    json out = {
        {"id", job.id},
        {"source", job.request.source},
        {"task_id", job.request.task_id},
        {"title", job.request.title},
        {"vault", job.request.vault},
        {"area", job.request.area},
        {"mode", job.request.mode == Mode::Run ? "run" : "prefill"},
        {"kind", job.request.kind},
        {"private", job.is_private},
        {"status", status_name(job.status)},
        {"model", job.model},
        {"created", job.created},
        {"updated", job.updated},
    };
    if (!job.error.empty()) {
        out["error"] = job.error;
    }
    if (!job.runner.empty()) {
        out["runner"] = job.runner;
    }
    if (!job.note.empty()) {
        out["note"] = job.note;
    }
    if (!job.brief.is_null()) {
        out["brief"] = job.brief;
    }
    if (with_text && !job.draft.is_null()) {
        out["draft"] = job.draft;
    }
    if (with_text && !job.draft_error.empty()) {
        out["draft_error"] = job.draft_error;
    }
    if (with_text && !job.summary.is_null()) {
        out["summary"] = job.summary;
    }
    if (with_text && !job.summary_error.empty()) {
        out["summary_error"] = job.summary_error;
    }
    if (with_text) {
        out["markdown"] = job.request.markdown;
        out["output"] = job.output;
        out["output_truncated"] = job.output_truncated;
    }
    return out;
}

std::string new_id() {
    static std::mutex mutex;
    static std::mt19937_64 engine{[] {
        std::random_device device;
        return (static_cast<std::uint64_t>(device()) << 32) ^ device() ^
               static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    }()};
    std::lock_guard<std::mutex> lock(mutex);
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(engine()));
    return text;
}

// ----- Store -------------------------------------------------------------------

Job* Store::find_locked(const std::string& id) {
    for (auto& job : jobs_) {
        if (job.id == id) {
            return &job;
        }
    }
    return nullptr;
}

std::string Store::add(Request request, bool is_private) {
    Job job;
    job.id = new_id();
    job.request = std::move(request);
    job.is_private = is_private;
    job.created = job.updated = usage::now_iso8601();
    std::lock_guard<std::mutex> lock(mutex_);
    while (jobs_.size() >= kMaxJobs) {
        auto oldest =
            std::find_if(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.status != Status::Running; });
        if (oldest == jobs_.end()) {
            break;  // cannot happen while kMaxRunning < kMaxJobs
        }
        jobs_.erase(oldest);
    }
    jobs_.push_back(std::move(job));
    return jobs_.back().id;
}

std::optional<Job> Store::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& job : jobs_) {
        if (job.id == id) {
            return job;
        }
    }
    return std::nullopt;
}

std::vector<Job> Store::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {jobs_.begin(), jobs_.end()};
}

bool Store::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& j) { return j.id == id; });
    if (it == jobs_.end()) {
        return false;
    }
    jobs_.erase(it);
    return true;
}

config::Status Store::start(const std::string& id, const std::string& model,
                            const std::optional<std::string>& markdown, std::uint64_t* run) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job* job = find_locked(id);
    if (!job) {
        return {404, "no such job"};
    }
    if (job->status == Status::Running) {
        return {409, "the job is running already"};
    }
    const auto running =
        std::count_if(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.status == Status::Running; });
    if (static_cast<std::size_t>(running) >= kMaxRunning) {
        return {409, "already " + std::to_string(kMaxRunning) + " jobs running; wait until one is done"};
    }
    if (markdown) {
        job->request.markdown = *markdown;
    }
    job->status = Status::Running;
    job->run += 1;
    if (run) {
        *run = job->run;
    }
    job->model = model;
    job->output.clear();
    job->output_truncated = false;
    job->error.clear();
    job->runner.clear();
    job->note.clear();
    job->draft = nullptr;
    job->draft_error.clear();
    job->summary = nullptr;
    job->summary_error.clear();
    job->brief = nullptr;
    job->updated = usage::now_iso8601();
    return {};
}

config::Status Store::hold(const std::string& id, const std::string& model) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job* job = find_locked(id);
    if (!job) {
        return {404, "no such job"};
    }
    if (job->status == Status::Running) {
        // Started by another request since the caller looked: its run goes on, nothing waits for an answer.
        return {409, "the job is running already"};
    }
    job->status = Status::NeedsConfirmation;
    job->model = model;
    job->updated = usage::now_iso8601();
    return {};
}

bool Store::append(const std::string& id, std::uint64_t run, const std::string& delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job* job = find_locked(id);
    if (!job || job->status != Status::Running || job->run != run) {
        return false;
    }
    if (job->output_truncated) {
        return true;
    }
    if (job->output.size() + delta.size() > kMaxOutputBytes) {
        // Cut at a character boundary: back off over UTF-8 continuation bytes.
        std::size_t room = kMaxOutputBytes - job->output.size();
        while (room > 0 && room < delta.size() && (static_cast<unsigned char>(delta[room]) & 0xC0) == 0x80) {
            --room;
        }
        job->output.append(delta, 0, room);
        job->output_truncated = true;
        return true;
    }
    job->output += delta;
    return true;
}

void Store::finish(const std::string& id, std::uint64_t run, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job* job = find_locked(id);
    if (!job || job->status != Status::Running || job->run != run) {
        return;
    }
    job->status = error.empty() ? Status::Done : Status::Failed;
    job->error = error;
    job->updated = usage::now_iso8601();
}

void Store::set_draft(const std::string& id, std::uint64_t run, const json& draft, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job* job = find_locked(id);
    if (!job || job->status != Status::Running || job->run != run) {
        return;
    }
    job->draft = draft;
    job->draft_error = error;
}

void Store::set_summary(const std::string& id, std::uint64_t run, const json& summary, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job* job = find_locked(id);
    if (!job || job->status != Status::Running || job->run != run) {
        return;
    }
    job->summary = summary;
    job->summary_error = error;
}

void Store::describe(const std::string& id, std::uint64_t run, const std::string& runner, const std::string& note,
                     const json& brief) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job* job = find_locked(id);
    if (!job || job->status != Status::Running || job->run != run) {
        return;
    }
    job->runner = runner;
    job->note = note;
    job->brief = brief;
    job->updated = usage::now_iso8601();
}

config::Status Store::cancel(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job* job = find_locked(id);
    if (!job) {
        return {404, "no such job"};
    }
    if (job->status != Status::Running) {
        return {409, "the job does not run"};
    }
    job->status = Status::Cancelled;
    job->updated = usage::now_iso8601();
    return {};
}

void Store::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.clear();
}

Store& store() {
    // Never destroyed: a run thread may still write into it while the process ends.
    static Store* instance = new Store;
    return *instance;
}

// ----- RateLimiter -------------------------------------------------------------

bool RateLimiter::allow(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!calls_.empty() && now - calls_.front() >= window_) {
        calls_.pop_front();
    }
    if (calls_.size() >= limit_) {
        return false;
    }
    calls_.push_back(now);
    return true;
}

long RateLimiter::retry_after(std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (calls_.size() < limit_ || calls_.empty()) {
        return 1;
    }
    const auto wait = std::chrono::duration_cast<std::chrono::seconds>(calls_.front() + window_ - now).count();
    return wait < 1 ? 1 : static_cast<long>(wait) + 1;
}

// ----- the text of a run -------------------------------------------------------

std::string run_prompt(const Request& request) {
    return "Task " + request.task_id + " (kind " + request.kind + ", area " + request.area + ", vault " +
           request.vault + "), handed over by " +
           request.source + ":\n\n" + request.markdown;
}

std::string run_system() {
    return "You are an agent of ai-desktop. The user handed you a task from their task vault (a Markdown file with a "
           "front matter and acceptance criteria). Work out how to do it: the steps, the files or places concerned, "
           "the open questions. Answer in the language of the task.";
}

}  // namespace ai_desktop::jobs
