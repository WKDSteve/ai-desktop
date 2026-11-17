#include "jobs_api.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

#include "api.hpp"
#include "config.hpp"
#include "control_secret.hpp"
#include "jobs.hpp"
#include "legacy.hpp"
#include "models.hpp"

using json = nlohmann::json;

namespace ai_desktop::jobs_api {

namespace {

using HooksPtr = std::shared_ptr<const Hooks>;

// Ids are 16 hex digits (jobs::new_id); the routes take nothing else.
constexpr const char* kIdPattern = "([0-9a-f]{16})";

// AI_DESKTOP_JOBS_PER_MINUTE (1-1000) replaces the default of jobs::kRateLimit; read once.
std::size_t rate_limit() {
    const char* text = legacy::env("AI_DESKTOP_JOBS_PER_MINUTE");
    if (!text) {
        return jobs::kRateLimit;
    }
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (*end != '\0' || value < 1 || value > 1000) {
        std::cerr << "ai-desktop: AI_DESKTOP_JOBS_PER_MINUTE must be 1-1000; using " << jobs::kRateLimit << "\n";
        return jobs::kRateLimit;
    }
    return static_cast<std::size_t>(value);
}

jobs::RateLimiter& post_limiter() {
    static jobs::RateLimiter limiter(rate_limit(), jobs::kRateWindow);
    return limiter;
}

// Every route of the jobs: the Host must be this server's (DNS rebinding: a page of a foreign name that resolves to
// 127.0.0.1 is same-origin to the browser and sends no Origin on a GET).
bool own_host(const httplib::Request& req, httplib::Response& res, int port) {
    if (!is_own_host(req.get_header_value("Host"), port)) {
        api::send_error(res, 403, "unknown host");
        return false;
    }
    return true;
}

const std::string& secret_of(const Hooks& hooks) {
    static const std::string none;
    return hooks.secret ? *hooks.secret : none;
}

// The routes that change a job from the dashboard: the page of this server (its Origin; the pre-routing handler of
// main.cpp has refused every other Origin) or a program with the control secret. A program without either is
// refused, so the confirmation of a private area cannot be given blind.
bool dashboard_or_secret(const httplib::Request& req, httplib::Response& res, const Hooks& hooks) {
    if (!req.get_header_value("Origin").empty()) {
        return true;
    }
    if (control_secret::matches(req.get_header_value("Authorization"), secret_of(hooks))) {
        return true;
    }
    api::send_error(res, 401, "the dashboard or the control secret is required");
    return false;
}

std::string model_or_default(const std::string& model) {
    return model.empty() ? config::effective("default_model") : model;
}

// Starts the run of a job that Store::start() moved to Running, on a thread of its own.
void spawn_run(const HooksPtr& hooks, const std::string& id) {
    const auto job = jobs::store().get(id);
    if (!job) {
        return;
    }
    const std::string prompt = jobs::run_prompt(job->request);
    const std::string model = job->model;
    hooks->event("job", "jobs", id);
    std::thread([hooks, id, prompt, model] {
        std::string error;
        try {
            error = hooks->run(prompt, jobs::run_system(), model,
                               [&id](const std::string& delta) { jobs::store().append(id, delta); });
        } catch (const std::exception& e) {
            error = std::string("ai-desktop: ") + e.what();
        } catch (...) {
            error = "ai-desktop: unknown internal error";
        }
        jobs::store().finish(id, error);
        hooks->event("job", "jobs", id);
    }).detach();
}

// Runs a job with `model` if the rule of private areas lets it; else holds it for the person. Returns the status
// to answer with (0 = ok).
config::Status run_or_hold(const HooksPtr& hooks, const std::string& id, const std::string& model, bool confirmed,
                           const std::optional<std::string>& markdown) {
    const auto job = jobs::store().get(id);
    if (!job) {
        return {404, "no such job"};
    }
    const bool local = models::resolve(model).local;
    if (!confirmed && !jobs::may_send(job->is_private, local)) {
        // hold() refuses a job that runs (also one started since the get() above); only a held job is announced.
        if (jobs::store().hold(id, model).ok()) {
            hooks->event("job", "jobs", id);
        }
        return {409, "the area '" + job->request.area +
                         "' is private: it goes to a local model only, or after you confirmed it"};
    }
    config::Status st = jobs::store().start(id, model, markdown);
    if (!st.ok()) {
        return st;
    }
    spawn_run(hooks, id);
    return {};
}

}  // namespace

bool is_own_host(const std::string& host, int port) {
    const std::string suffix = ":" + std::to_string(port);
    for (const char* name : {"127.0.0.1", "localhost", "[::1]"}) {
        if (host == std::string(name) + suffix) {
            return true;
        }
    }
    return false;
}

void register_routes(httplib::Server& server, Hooks plain_hooks) {
    const auto hooks = std::make_shared<const Hooks>(std::move(plain_hooks));

    // A family app hands over a task. The order of the checks: the place (Host), that no browser sent it, the form
    // and size, the rate, the secret, the setting, the fields.
    server.Post("/jobs", [hooks](const httplib::Request& req, httplib::Response& res) {
        if (!own_host(req, res, hooks->port)) {
            return;
        }
        // A browser names the page that sent a request (Origin, Sec-Fetch-*) and a page cannot take that away. A
        // family app is a program and sends neither: a job never comes from a web page, the dashboard included.
        if (!req.get_header_value("Origin").empty() || req.has_header("Sec-Fetch-Site") ||
            req.has_header("Sec-Fetch-Mode")) {
            api::send_error(res, 403, "jobs are not taken from a web page");
            return;
        }
        if (req.get_header_value("Content-Type").rfind("application/json", 0) != 0) {
            api::send_error(res, 415, "Content-Type must be application/json");
            return;
        }
        if (req.body.size() > jobs::kMaxBodyBytes) {
            api::send_error(res, 413, "a job must not exceed " + std::to_string(jobs::kMaxBodyBytes / 1024) + " KiB");
            return;
        }
        if (secret_of(*hooks).empty()) {
            api::send_error(res, 503, "no control secret in force: ai-desktop could not write it at start");
            return;
        }
        if (!control_secret::matches(req.get_header_value("Authorization"), secret_of(*hooks))) {
            api::send_error(res, 401, "the control secret of ai-desktop is required");
            return;
        }
        // Counted after the secret: a program without it cannot use up the jobs of the family apps (and cannot
        // guess 256 bits either way).
        if (!post_limiter().allow()) {
            res.set_header("Retry-After", std::to_string(post_limiter().retry_after()));
            api::send_error(res, 429, "too many jobs; try again later");
            return;
        }
        if (config::effective("jobs") != "on") {
            api::send_error(res, 403, "jobs from other apps are turned off in the settings of ai-desktop");
            return;
        }
        const json body = json::parse(req.body, nullptr, false);
        jobs::Request request;
        if (const auto st = jobs::parse_request(body, request); !st.ok()) {
            api::send_error(res, st.http, st.message);
            return;
        }
        const bool is_private = jobs::is_private(request.area, request.private_hint);
        const bool run = request.mode == jobs::Mode::Run;
        const std::string model = model_or_default(request.model);
        const std::string id = jobs::store().add(std::move(request), is_private);
        json out = {{"job_id", id}, {"private", is_private}};
        if (run) {
            const config::Status st = run_or_hold(hooks, id, model, false, std::nullopt);
            if (!st.ok()) {
                // Held for the person (private area) or not started (too many runs): the job stands in the
                // dashboard either way, so the hand-over itself succeeded.
                out["note"] = st.message;
            }
        } else {
            hooks->event("job", "jobs", id);
        }
        if (const auto job = jobs::store().get(id)) {
            out["status"] = jobs::status_name(job->status);
        }
        hooks->show(id);
        api::send_json(res, 201, out);
    });

    server.Get("/jobs", [hooks](const httplib::Request& req, httplib::Response& res) {
        if (!own_host(req, res, hooks->port)) {
            return;
        }
        json list = json::array();
        for (const auto& job : jobs::store().list()) {
            list.push_back(jobs::to_json(job, false));
        }
        api::send_json(res, 200, {{"jobs", list}, {"enabled", config::effective("jobs") == "on"}});
    });

    server.Get(std::string("/jobs/") + kIdPattern, [hooks](const httplib::Request& req, httplib::Response& res) {
        if (!own_host(req, res, hooks->port)) {
            return;
        }
        const auto job = jobs::store().get(req.matches[1].str());
        if (!job) {
            api::send_error(res, 404, "no such job");
            return;
        }
        api::send_json(res, 200, jobs::to_json(*job, true));
    });

    // {model?, markdown?, confirm_private?}: the dashboard runs a job, with the text as the person left it.
    const std::string run_path = std::string("/jobs/") + kIdPattern + "/run";
    server.Post(run_path, [hooks](const httplib::Request& req, httplib::Response& res) {
        if (!own_host(req, res, hooks->port) || !dashboard_or_secret(req, res, *hooks)) {
            return;
        }
        if (req.body.size() > jobs::kMaxBodyBytes) {
            api::send_error(res, 413, "a job must not exceed " + std::to_string(jobs::kMaxBodyBytes / 1024) + " KiB");
            return;
        }
        json body = req.body.empty() ? json::object() : json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            api::send_error(res, 400, "the request body must be a JSON object");
            return;
        }
        std::string model;
        if (body.contains("model") && !body["model"].is_null()) {
            if (!body["model"].is_string() || body["model"].get<std::string>().size() > jobs::kMaxModelBytes ||
                !jobs::text_error(body["model"].get<std::string>(), false).empty()) {
                api::send_error(res, 400, "'model' must be a model name of one line");
                return;
            }
            model = body["model"].get<std::string>();
        }
        std::optional<std::string> markdown;
        if (body.contains("markdown") && !body["markdown"].is_null()) {
            if (!body["markdown"].is_string()) {
                api::send_error(res, 400, "'markdown' must be a string");
                return;
            }
            markdown = body["markdown"].get<std::string>();
            if (markdown->size() > jobs::kMaxMarkdownBytes) {
                api::send_error(res, 400, "'markdown' is too long");
                return;
            }
            if (const std::string error = jobs::text_error(*markdown, true); !error.empty()) {
                api::send_error(res, 400, "'markdown': " + error);
                return;
            }
        }
        const bool confirmed = body.contains("confirm_private") && body["confirm_private"] == true;
        const std::string id = req.matches[1].str();
        const config::Status st = run_or_hold(hooks, id, model_or_default(model), confirmed, markdown);
        if (!st.ok()) {
            json error = {{"error", {{"message", st.message}}}};
            if (const auto job = jobs::store().get(id)) {
                error["status"] = jobs::status_name(job->status);
            }
            api::send_json(res, st.http, error);
            return;
        }
        const auto job = jobs::store().get(id);
        if (!job) {
            api::send_error(res, 404, "no such job");  // put away while it started
            return;
        }
        api::send_json(res, 200, jobs::to_json(*job, true));
    });

    server.Delete(std::string("/jobs/") + kIdPattern, [hooks](const httplib::Request& req, httplib::Response& res) {
        if (!own_host(req, res, hooks->port) || !dashboard_or_secret(req, res, *hooks)) {
            return;
        }
        if (!jobs::store().remove(req.matches[1].str())) {
            api::send_error(res, 404, "no such job");
            return;
        }
        res.status = 204;
    });
}

}  // namespace ai_desktop::jobs_api
