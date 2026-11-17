#pragma once

#include <functional>
#include <string>

#include "httplib.h"

// The routes of the jobs (docs/JOBS.md):
//   POST   /jobs           a family app hands over a task: the control secret only, never from a web page
//   GET    /jobs           the jobs, without their text (the dashboard)
//   GET    /jobs/<id>      one job with its text and output
//   POST   /jobs/<id>/run  run it, from the dashboard (or with the control secret)
//   DELETE /jobs/<id>      put it away, likewise
namespace ai_desktop::jobs_api {

using OnDelta = std::function<void(const std::string&)>;

struct Hooks {
    int port = 0;
    // The control secret in force, owned by main.cpp and set before the server takes requests (read-only after);
    // null or empty: POST /jobs answers 503 (the secret could not be written at start).
    const std::string* secret = nullptr;
    // Runs a prompt against a model, streaming its answer; returns "" or the error.
    std::function<std::string(const std::string& prompt, const std::string& system, const std::string& model,
                              const OnDelta& on_delta)>
        run;
    // An event for the dashboard (the event queue of /events).
    std::function<void(const std::string& type, const std::string& agent, const std::string& message)> event;
    // Brings the dashboard to the front for a new job.
    std::function<void(const std::string& job_id)> show;
};

// Is `host` (the Host header) this server's: 127.0.0.1, localhost or [::1] with the port it listens on? A name
// that resolves to the loopback address (DNS rebinding) is not.
bool is_own_host(const std::string& host, int port);

void register_routes(httplib::Server& server, Hooks hooks);

}  // namespace ai_desktop::jobs_api
