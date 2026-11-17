#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "config.hpp"
#include "nlohmann/json.hpp"

// Jobs from the other apps of the family (tasks-desktop, tasks.nvim): a task, as Markdown, handed to ai-desktop either
// to stand in the dashboard as a new job (`prefill`) or to run at once (`run`). Pure logic, no HTTP: the validation of
// a request, the store of the jobs, the rate limit and the rule for private areas. The routes are in jobs_api.cpp,
// the threat model in docs/JOBS.md.
namespace ai_desktop::jobs {

// Bounds of a request. The body as a whole is bounded before it is parsed (kMaxBodyBytes); each field once more.
constexpr std::size_t kMaxBodyBytes = 320 * 1024;
constexpr std::size_t kMaxMarkdownBytes = 256 * 1024;
constexpr std::size_t kMaxTitleBytes = 300;
constexpr std::size_t kMaxSourceBytes = 64;
constexpr std::size_t kMaxTaskIdBytes = 200;
constexpr std::size_t kMaxAreaBytes = 100;
constexpr std::size_t kMaxVaultBytes = 1024;
constexpr std::size_t kMaxModelBytes = 200;
// How many jobs the store keeps (the oldest that is not running goes first), and how many run at the same time.
constexpr std::size_t kMaxJobs = 100;
constexpr std::size_t kMaxRunning = 2;
// The output of a run is kept up to this size; the rest is cut off (and the job says so).
constexpr std::size_t kMaxOutputBytes = 512 * 1024;
// POST /jobs: at most this many in a window of this length.
constexpr std::size_t kRateLimit = 10;
constexpr std::chrono::seconds kRateWindow{60};

enum class Mode { Prefill, Run };

// What the job asks for. `handoff` (the default) hands over a task to work on; the others are named now so the
// channel stays one, and get their own prompts in tasks of their own: until then they run with the prompt of a
// hand-over and the dashboard shows the kind.
constexpr const char* kKinds[] = {"handoff", "review-done", "draft-task", "prepare-decision", "split-plan"};
bool valid_kind(const std::string& kind);

// What a family app sends. `markdown` is the task file; the other fields say where it comes from.
struct Request {
    std::string source;    // the app: [a-z0-9._-], e.g. "tasks-desktop", "tasks.nvim"
    std::string task_id;   // "<area>/<slug>": [A-Za-z0-9._/-], no "..", no leading or trailing "/"
    std::string title;     // one line
    std::string markdown;  // the task as Markdown: lines, tabs; no other control or invisible character
    std::string vault;     // the folder of the vault, one line (a path; only shown and passed on, never opened)
    std::string area;      // [A-Za-z0-9._-]
    Mode mode = Mode::Prefill;
    std::string kind = "handoff";  // one of kKinds
    std::string model;     // optional: the model of a run; "" = default_model of the settings
    bool private_hint = false;  // the caller knows the area is private
};

// Checks text against the rules of tools/check-control-chars.py: valid UTF-8, no C0 control but (with
// `multiline`) line feed, carriage return and tab, no DEL, no C1, no soft hyphen, no zero-width, line or paragraph
// separator, bidirectional control or byte order mark. Returns "" or what is wrong.
std::string text_error(const std::string& text, bool multiline);

// Parses and validates a request body. 400 with a message naming the field, or ok.
config::Status parse_request(const nlohmann::json& body, Request& out);

// The areas marked private in the settings ("private_areas": names separated by commas).
std::vector<std::string> private_areas();

// Validates the value of the setting "private_areas"; normalises it in place (trimmed names, one comma between).
config::Status normalize_private_areas(std::string& value);

// Does the request's area count as private (named in the settings, or the caller said so)?
bool is_private(const std::string& area, bool private_hint);

// May a job of this area go to this model without asking? A private area only to a local model.
bool may_send(bool is_private_job, bool model_is_local);

enum class Status { Prefilled, NeedsConfirmation, Running, Done, Failed };
const char* status_name(Status status);

struct Job {
    std::string id;
    Request request;
    bool is_private = false;
    Status status = Status::Prefilled;
    std::string model;       // the model of the run (once one was asked for)
    std::string output;      // the answer of the run
    bool output_truncated = false;
    std::string error;       // why a run failed
    std::string created;     // UTC, YYYY-MM-DDTHH:MM:SSZ
    std::string updated;
};

// The JSON the dashboard reads; `with_text` adds markdown and output (the list leaves them out).
nlohmann::json to_json(const Job& job, bool with_text);

// A random id of 16 hex digits (not a secret: it names a job, the routes need no knowledge of it to be safe).
std::string new_id();

// The jobs in memory, newest last. Thread-safe. Nothing is written to disk: a job holds a task, which may be private.
class Store {
public:
    // Adds a job; drops the oldest job that is not running when the store is full. Returns the id.
    std::string add(Request request, bool is_private);
    std::optional<Job> get(const std::string& id) const;
    std::vector<Job> list() const;
    bool remove(const std::string& id);

    // Moves a job to Running with `model` (and, from the dashboard, an edited text). 404 unknown, 409 when it
    // runs already or kMaxRunning jobs run.
    config::Status start(const std::string& id, const std::string& model, const std::optional<std::string>& markdown);
    // Marks a job that waits for the person (a private area and a model that is not local). 404 unknown, 409 when
    // it runs (a running job is never put back to waiting: a second start would run it twice).
    config::Status hold(const std::string& id, const std::string& model);
    // Appends to the output of a running job (cut at kMaxOutputBytes). False when the job is gone.
    bool append(const std::string& id, const std::string& delta);
    void finish(const std::string& id, const std::string& error);

    void clear();

private:
    mutable std::mutex mutex_;
    std::deque<Job> jobs_;
    Job* find_locked(const std::string& id);
};

Store& store();

// A sliding window: at most `limit` calls in `window`.
class RateLimiter {
public:
    RateLimiter(std::size_t limit, std::chrono::steady_clock::duration window) : limit_(limit), window_(window) {}
    // True and counted when the call is allowed; false (not counted) otherwise.
    bool allow(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    // Seconds until the next call is allowed (at least 1).
    long retry_after(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;

private:
    mutable std::mutex mutex_;
    std::size_t limit_;
    std::chrono::steady_clock::duration window_;
    std::deque<std::chrono::steady_clock::time_point> calls_;
};

// The prompt and the system text of a run.
std::string run_prompt(const Request& request);
std::string run_system();

}  // namespace ai_desktop::jobs
