// Brings the dashboard to the front when a family app hands over a new job.

#pragma once

#include <string>

// Brings the dashboard to the front for a new job. Windows does not let a process in the background take the focus
// (the foreground lock); the app that hands over the job holds it and calls AllowSetForegroundWindow(<pid of
// ai-desktop>) before it sends the job (docs/JOBS.md). With that right:
//   * a window whose title is the dashboard's ("ai Dashboard" / "KI Dashboard": the tab is open and shown) is
//     restored and brought to the front, and the page selects the job from the event it gets;
//   * else the dashboard is opened at http://127.0.0.1:<port>/#job=<id> in the default browser, which may come to
//     the front because ai-desktop passes its right on (AllowSetForegroundWindow(ASFW_ANY)).
// Without the right Windows flashes the task bar button instead, which is what it should do. Elsewhere the page is
// opened with xdg-open (Linux, with a display) or open (macOS). AI_DESKTOP_JOBS_FOREGROUND=0 turns all of it off
// (the tests). Runs on a thread of its own and returns at once.
namespace ai_desktop::foreground {

void show_job(int port, const std::string& job_id);

}  // namespace ai_desktop::foreground
