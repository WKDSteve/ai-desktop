#include <chrono>
#include <filesystem>
#include <string>

#include "agent.hpp"
#include "minitest.hpp"
#include "process.hpp"

namespace fs = std::filesystem;
namespace process = ai_desktop::process;

#ifndef AI_DESKTOP_FAKE_TASKS
#error "AI_DESKTOP_FAKE_TASKS (the path of tests/doubles/fake_tasks) is set by CMakeLists.txt"
#endif

namespace {

process::Spec fake(std::vector<std::string> args) {
    process::Spec spec;
    spec.program = fs::u8path(AI_DESKTOP_FAKE_TASKS);
    spec.args = std::move(args);
    return spec;
}

}  // namespace

TEST(process_quotes_arguments_for_windows) {
    CHECK_EQ(process::quote_windows_arg("plain"), std::string("plain"));
    CHECK_EQ(process::quote_windows_arg(""), std::string("\"\""));
    CHECK_EQ(process::quote_windows_arg("a b"), std::string("\"a b\""));
    CHECK_EQ(process::quote_windows_arg("say \"hi\""), std::string("\"say \\\"hi\\\"\""));
    CHECK_EQ(process::quote_windows_arg("C:\\dir with space\\"), std::string("\"C:\\dir with space\\\\\""));
    CHECK_EQ(process::quote_windows_arg("C:\\no\\space"), std::string("C:\\no\\space"));
}

TEST(process_arguments_arrive_as_they_were) {
    const std::vector<std::string> args = {"echo-args", "a b", "say \"hi\"", "C:\\dir with space\\", "", "Bash(tasks:*)"};
    // (ASCII only: a child that reads argv in the C runtime gets it in the system code page on Windows)
    std::string out;
    std::string error;
    CHECK_EQ(process::run_capture(fake(args), std::chrono::seconds(10), 4096, out, &error), 0);
    std::string expected;
    for (std::size_t i = 1; i < args.size(); ++i) expected += args[i] + "\n";
    CHECK_EQ(out, expected);
}

TEST(process_environment_is_set_for_the_child) {
    auto spec = fake({"echo-env", "AI_DESKTOP_PROCESS_TEST"});
    spec.env = {{"AI_DESKTOP_PROCESS_TEST", "value with space"}};
    std::string out;
    CHECK_EQ(process::run_capture(spec, std::chrono::seconds(10), 4096, out, nullptr), 0);
    CHECK_EQ(out, std::string("value with space\n"));
}

TEST(process_a_child_that_takes_too_long_is_ended) {
    std::string out;
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    CHECK_EQ(process::run_capture(fake({"sleep", "20000"}), std::chrono::milliseconds(300), 4096, out, &error), -2);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
    CHECK(!error.empty());
}

TEST(process_a_missing_program_is_an_error) {
    process::Spec spec;
    spec.program = fs::temp_directory_path() / "ai-desktop-no-such-program.exe";
    std::string out;
    std::string error;
    CHECK_EQ(process::run_capture(spec, std::chrono::seconds(5), 4096, out, &error), -3);
    CHECK(!error.empty());
}

TEST(process_the_brief_of_a_tasks_command_is_read) {
    ai_desktop::agent::TasksCommand tasks;
    tasks.program = fs::u8path(AI_DESKTOP_FAKE_TASKS);
    ai_desktop::agent::Brief brief;
    CHECK(ai_desktop::agent::fetch_brief(tasks, "ai-desktop:0123456789abcdef", brief).ok());
    CHECK_EQ(brief.brief_version, 2);
    CHECK(brief.markdown.find("--by=ai-desktop:0123456789abcdef") != std::string::npos);
    CHECK(brief.checks_source.empty());  // without --repo the stand-in, like tasks.nvim, has no checks

    // With the run's worktree the brief has the checks of that repository (the stand-in sees --repo).
    ai_desktop::agent::Brief with_repo;
    CHECK(ai_desktop::agent::fetch_brief(tasks, "ai-desktop:0123456789abcdef", with_repo, fs::temp_directory_path()).ok());
    CHECK_EQ(with_repo.checks_source, std::string("testing"));
    CHECK(with_repo.markdown.find("--repo=" + fs::temp_directory_path().u8string()) != std::string::npos);
}

#ifndef _WIN32
#include <csignal>
#include <pthread.h>

// main.cpp blocks SIGINT and SIGTERM in the server's threads (a thread of its own waits for them); a child started
// from such a thread must not keep that mask across execve, or Claude Code, git and tasks would ignore a SIGTERM.
TEST(process_child_does_not_inherit_blocked_signals) {
    sigset_t block;
    sigset_t old;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    process::Spec spec;
    spec.program = "/bin/sh";
    spec.args = {"-c", "kill -TERM $$; echo survived"};
    std::string out;
    std::string error;
    const int code = process::run_capture(spec, std::chrono::seconds(10), 4096, out, &error);
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    CHECK(out.find("survived") == std::string::npos);
    CHECK_EQ(code, -1);  // ended by the signal
}
#endif
