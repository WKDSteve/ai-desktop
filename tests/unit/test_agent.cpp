#include <algorithm>
#include <filesystem>
#include <string>

#include "agent.hpp"
#include "config.hpp"
#include "minitest.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace agent = ai_desktop::agent;
namespace config = ai_desktop::config;

namespace {

json brief_doc() {
    return {{"schema", "tasks-agent-brief/1"},
            {"brief_version", 2},
            {"contract_schema", 1},
            {"engine_version", "0.1.0"},
            {"markdown", "# Tasks family\n\nclaim first\n"}};
}

}  // namespace

TEST(agent_model_names) {
    CHECK(agent::is_agent_model("claude-code"));
    CHECK(agent::is_agent_model("claude-code:sonnet"));
    CHECK(!agent::is_agent_model("claude-code:"));
    CHECK(!agent::is_agent_model("claude-haiku-5-5"));
    CHECK(!agent::is_agent_model(""));
    CHECK_EQ(agent::submodel("claude-code:opus"), std::string("opus"));
    CHECK_EQ(agent::submodel("claude-code"), std::string(""));
}

TEST(agent_brief_is_checked) {
    agent::Brief brief;
    CHECK(agent::parse_brief(brief_doc().dump(), brief).ok());
    CHECK_EQ(brief.brief_version, 2);
    CHECK_EQ(brief.engine_version, std::string("0.1.0"));
    CHECK(brief.note.empty());
    CHECK(brief.checks_source.empty());

    json newer = brief_doc();
    newer["brief_version"] = 3;
    CHECK(agent::parse_brief(newer.dump(), brief).ok());
    CHECK(!brief.note.empty());

    // Brief 1 has no checks of a repository: refused like an older contract.
    json first = brief_doc();
    first["brief_version"] = 1;
    CHECK_EQ(agent::parse_brief(first.dump(), brief).http, 422);

    json checks = brief_doc();
    checks["checks"] = {{"source", "testing"}, {"land", false}, {"per_commit", "testing ci commit <sha> --quick -- --cached"}};
    CHECK(agent::parse_brief(checks.dump(), brief).ok());
    CHECK_EQ(brief.checks_source, std::string("testing"));

    json older = brief_doc();
    older["contract_schema"] = 0;
    CHECK_EQ(agent::parse_brief(older.dump(), brief).http, 422);

    json other = brief_doc();
    other["schema"] = "tasks-agent-brief/2";
    CHECK_EQ(agent::parse_brief(other.dump(), brief).http, 422);

    json empty = brief_doc();
    empty["markdown"] = "  \n";
    CHECK_EQ(agent::parse_brief(empty.dump(), brief).http, 422);

    json hostile = brief_doc();
    hostile["markdown"] = "bidi \xe2\x80\xae";
    CHECK_EQ(agent::parse_brief(hostile.dump(), brief).http, 422);

    CHECK_EQ(agent::parse_brief("tasks: unknown command 'agent-brief'", brief).http, 422);
    json missing = brief_doc();
    missing.erase("brief_version");
    CHECK_EQ(agent::parse_brief(missing.dump(), brief).http, 422);
}

TEST(agent_works_in_the_repository_of_the_area_else_in_the_vault) {
    const fs::path root = minitest::make_temp_dir("agent-workdir");
    const fs::path vault = root / "tasks-vault";
    fs::create_directories(vault);
    fs::create_directories(root / "with-repo" / ".git");
    fs::create_directories(root / "no-repo");
    std::string error;
    CHECK(agent::work_dir(vault.u8string(), "with-repo", &error) == root / "with-repo");
    CHECK(agent::work_dir(vault.u8string(), "no-repo", &error) == vault);
    CHECK(agent::work_dir(vault.u8string(), "missing", &error) == vault);
    CHECK(agent::work_dir((root / "nope").u8string(), "with-repo", &error).empty());
    CHECK(!error.empty());
    CHECK(agent::work_dir("relative/vault", "x", &error).empty());
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

TEST(agent_claude_args_limit_the_tools) {
    const auto args = agent::claude_args("/tmp/brief.md", "/vault", "sonnet");
    const auto has = [&](const std::string& a) { return std::find(args.begin(), args.end(), a) != args.end(); };
    CHECK(has("-p"));
    CHECK(has("stream-json"));
    CHECK(has("--append-system-prompt-file"));
    CHECK(has("--model"));
    CHECK(has("Bash(tasks:*)"));
    // Exactly the commands of the checks, nothing wider (no `ci:*`, no command a CLAUDE.md of the repository names).
    CHECK((agent::allowed_tools() ==
           std::vector<std::string>{"Bash(tasks:*)", "Bash(git:*)", "Bash(scripts/test.sh ci commit:*)",
                                    "Bash(testing ci commit:*)", "Bash(scripts/test.sh ci land)",
                                    "Bash(testing ci land)", "Read", "Edit", "Write", "Glob", "Grep"}));
    const auto list = std::find(args.begin(), args.end(), "--allowedTools");
    CHECK(list != args.end() && std::vector<std::string>(list + 1, args.end()) == agent::allowed_tools());
    // Refused although Bash(git:*) allows git (decision of 2026-11-21): the pre-push hook skipped, a config value
    // for one call. The list stands right before --allowedTools, which ends it.
    CHECK((agent::disallowed_tools() == std::vector<std::string>{"Bash(git push --no-verify:*)", "Bash(git -c:*)"}));
    const auto refused = std::find(args.begin(), args.end(), "--disallowedTools");
    CHECK(refused != args.end() && list != args.end() && refused < list &&
          std::vector<std::string>(refused + 1, list) == agent::disallowed_tools());
    CHECK(!has("--dangerously-skip-permissions"));
    CHECK(!has("bypassPermissions"));
    CHECK_EQ(args.back(), agent::allowed_tools().back());  // the tool list is last
    // The session has only the built-in tools of the allowed list (not Task, Workflow, Monitor, EnterWorktree, ...)
    // and no MCP server of the person's settings.
    const auto tools = std::find(args.begin(), args.end(), "--tools");
    CHECK(tools != args.end() && tools + 1 != args.end());
    if (tools != args.end() && tools + 1 != args.end()) {
        CHECK_EQ(*(tools + 1), std::string("Bash,Read,Edit,Write,Glob,Grep"));
        CHECK(tools + 2 != args.end() && (tools + 2)->rfind("--", 0) == 0);  // the variadic list ends there
    }
    CHECK(has("--strict-mcp-config"));
    const auto plain = agent::claude_args("/tmp/brief.md", "/vault", "");
    CHECK(std::find(plain.begin(), plain.end(), "--model") == plain.end());
}

TEST(agent_does_not_inherit_a_claude_code_session) {
    CHECK(agent::session_env_removals({"PATH", "CLAUDE_CODE_USE_BEDROCK", "ANTHROPIC_BASE_URL"}).empty());
    const auto removed = agent::session_env_removals(
        {"PATH", "CLAUDECODE", "CLAUDE_CODE_SESSION_ID", "CLAUDE_AGENT_SDK_VERSION", "ANTHROPIC_BASE_URL",
         "USE_STAGING_OAUTH", "ANTHROPIC_API_KEY", "TASKS_VAULT", "CLAUDE_PID"});
    std::vector<std::string> names;
    for (const auto& [name, value] : removed) {
        CHECK(value.empty());
        names.push_back(name);
    }
    CHECK((names == std::vector<std::string>{"CLAUDECODE", "CLAUDE_CODE_SESSION_ID", "CLAUDE_AGENT_SDK_VERSION",
                                             "ANTHROPIC_BASE_URL", "USE_STAGING_OAUTH", "CLAUDE_PID"}));
}

TEST(agent_prompt_names_the_task_and_the_claim) {
    ai_desktop::jobs::Request request;
    request.task_id = "ai-desktop/x";
    request.area = "ai-desktop";
    request.vault = "E:/vault";
    request.source = "tasks-desktop";
    request.markdown = "## Akzeptanz\n";
    const std::string text = agent::prompt(request, "ai-desktop:0123456789abcdef");
    CHECK(text.find("`tasks claim ai-desktop/x --by=ai-desktop:0123456789abcdef`") != std::string::npos);
    CHECK(text.find("tasks done ai-desktop/x") != std::string::npos);
    CHECK(text.find("## Akzeptanz") != std::string::npos);
    request.kind = "split-plan";
    CHECK(agent::prompt(request, "x").find("no instructions of its own") != std::string::npos);
}

TEST(agent_stream_json_becomes_output) {
    auto e = agent::render(R"({"type":"system","subtype":"init","model":"m","cwd":"/v"})");
    CHECK(e.text.find("Claude Code (m) in /v") != std::string::npos);
    e = agent::render(R"({"type":"assistant","message":{"content":[{"type":"text","text":"Hello"},{"type":"tool_use","name":"Bash","input":{"command":"tasks claim a/b --by=x"}}]}})");
    CHECK(e.text.find("Hello\n") != std::string::npos);
    CHECK(e.text.find("Bash: tasks claim a/b --by=x") != std::string::npos);
    e = agent::render(R"({"type":"user","message":{"content":[{"type":"tool_result","is_error":true,"content":"denied\nmore"}]}})");
    CHECK(e.text.find("denied") != std::string::npos);
    CHECK(e.text.find("more") == std::string::npos);
    e = agent::render(R"({"type":"user","message":{"content":[{"type":"tool_result","content":"fine"}]}})");
    CHECK(e.text.empty());
    e = agent::render(R"({"type":"result","subtype":"success","is_error":false,"usage":{"input_tokens":5,"output_tokens":7}})");
    CHECK(e.final);
    CHECK(!e.is_error);
    CHECK_EQ(e.input_tokens, 5L);
    CHECK_EQ(e.output_tokens, 7L);
    e = agent::render(R"({"type":"result","subtype":"error_max_turns","is_error":true})");
    CHECK(e.final);
    CHECK(e.is_error);
    CHECK_EQ(e.error, std::string("error_max_turns"));
    CHECK(agent::render("not json").text.empty());
    CHECK(!agent::render("[1,2]").final);
    // Foreign text keeps no control or bidi character.
    e = agent::render("{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"a\\u001b[31m\\u202eb\"}]}}");
    CHECK_EQ(ai_desktop::jobs::text_error(e.text, true), std::string(""));
}

// A line of JSON whose keys have other types than Claude Code writes is skipped like one that is no JSON: an
// exception out of render() would leave the reading of the run (and its cancel entry) behind.
TEST(agent_stream_json_of_another_form_is_skipped) {
    const char* lines[] = {
        R"({"type":5})",
        R"({"type":"system","subtype":["init"]})",
        R"({"type":"assistant","message":{"content":[{"type":"tool_use","name":null,"input":{}}]}})",
        R"({"type":"assistant","message":{"content":[{"type":7}]}})",
        R"({"type":"user","message":{"content":[{"type":"tool_result","is_error":"yes","content":"x"}]}})",
        R"({"type":"result","is_error":null})",
        R"({"type":"result","subtype":3})",
    };
    for (const char* line : lines) {
        bool threw = false;
        agent::Event e;
        try {
            e = agent::render(line);
        } catch (...) {
            threw = true;
        }
        CHECK(!threw);
        CHECK(e.text.empty());
        CHECK(!e.final);
    }
}

TEST(agent_finds_its_programs_from_the_settings) {
    const fs::path dir = minitest::make_temp_dir("agent-find");
    minitest::set_env("AI_DESKTOP_CONFIG_DIR", dir.string());
    config::reload();
    // A tasks.nvim checkout: bin/tasks beside ../scripts/tasks.lua runs through nvim (when nvim is on PATH).
    fs::create_directories(dir / "tasks.nvim" / "bin");
    fs::create_directories(dir / "tasks.nvim" / "scripts");
    minitest::write_file(dir / "tasks.nvim" / "bin" / "tasks", "#!/bin/sh\n");
    minitest::write_file(dir / "tasks.nvim" / "scripts" / "tasks.lua", "-- x\n");
    CHECK(config::update_settings({{"tasks_cli", (dir / "tasks.nvim" / "bin" / "tasks").u8string()}}).ok());
    const auto checkout = agent::find_tasks();
    CHECK(checkout.bin_dir == dir / "tasks.nvim" / "bin");
    if (checkout.ok()) {
        CHECK(checkout.prefix.size() == 8);
        CHECK_EQ(checkout.prefix.back(), (dir / "tasks.nvim" / "scripts" / "tasks.lua").u8string());
    } else {
        CHECK(checkout.error.find("nvim") != std::string::npos);
    }
    // A script that would need a shell is refused on Windows.
    minitest::write_file(dir / "tasks.cmd", "@echo off\n");
    CHECK(config::update_settings({{"tasks_cli", (dir / "tasks.cmd").u8string()}}).ok());
#ifdef _WIN32
    CHECK(!agent::find_tasks().ok());
#endif
    CHECK(config::update_settings({{"tasks_cli", (dir / "none.exe").u8string()}}).ok());
    CHECK(!agent::find_tasks().ok());
    CHECK_EQ(config::update_settings({{"tasks_cli", "relative/tasks"}}).http, 400);

    std::string error;
    CHECK(config::update_settings({{"claude_cli", (dir / "tasks.cmd").u8string()}}).ok());
#ifdef _WIN32
    CHECK(agent::find_claude(&error).empty());
    CHECK(error.find(".exe") != std::string::npos);
#endif
    CHECK(config::update_settings({{"claude_cli", (dir / "none.exe").u8string()}}).ok());
    CHECK(agent::find_claude(&error).empty());
    CHECK_EQ(config::effective("jobs_model"), std::string("claude-code"));

    config::reload();
    std::error_code ignored;
    fs::remove_all(dir, ignored);
}
