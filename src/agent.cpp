#include "agent.hpp"

#include <algorithm>
#include <cctype>

#include "process.hpp"

using json = nlohmann::json;

namespace ai_desktop::agent {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxBriefBytes = 512 * 1024;
constexpr std::size_t kSummaryChars = 200;

std::string trimmed(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

// One line, at most kSummaryChars bytes, cut at a character boundary.
std::string one_line(const std::string& text) {
    std::string line = text.substr(0, text.find('\n'));
    if (line.size() > kSummaryChars) {
        std::size_t cut = kSummaryChars;
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        line = line.substr(0, cut) + "…";
    }
    return line;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

fs::path setting_path(const char* key) {
    const std::string value = trimmed(config::effective(key));
    return value.empty() ? fs::path() : fs::u8path(value);
}

}  // namespace

bool is_agent_model(const std::string& model) {
    const std::string prefix = std::string(kModel) + ":";
    return model == kModel || (model.size() > prefix.size() && model.compare(0, prefix.size(), prefix) == 0);
}

std::string submodel(const std::string& model) {
    const std::size_t colon = model.find(':');
    return colon == std::string::npos ? "" : model.substr(colon + 1);
}

namespace {

// The command `name` of a Neovim plugin with a bin/ folder (tasks.nvim, testing.nvim): the setting `setting`, else
// `name` on PATH; a checkout's bin/<name> beside ../scripts/<name>.lua runs through nvim, any other file must be a
// program.
TasksCommand find_nvim_command(const char* setting, const std::string& name) {
    TasksCommand out;
    fs::path file = setting_path(setting);
    std::error_code ec;
    if (file.empty()) {
#ifdef _WIN32
        file = process::find_on_path({name + ".exe", name + ".cmd", name});
#else
        file = process::find_on_path({name});
#endif
        if (file.empty()) {
            out.error = "the " + name + " command is not on PATH (set " + setting + " in the settings)";
            return out;
        }
    } else if (!fs::is_regular_file(file, ec)) {
        out.error = std::string(setting) + " names no file: " + file.u8string();
        return out;
    }
    out.bin_dir = file.parent_path();
    const fs::path script = out.bin_dir.parent_path() / "scripts" / fs::u8path(name + ".lua");
    if (fs::is_regular_file(script, ec)) {
        // A checkout of the plugin: nvim runs the script, as bin/<name> does, without a shell in between.
#ifdef _WIN32
        out.program = process::find_on_path({"nvim.exe"});
#else
        out.program = process::find_on_path({"nvim"});
#endif
        if (out.program.empty()) {
            out.error = "nvim is not on PATH, and the " + name + " command of " + out.bin_dir.u8string() + " needs it";
            return out;
        }
        out.prefix = {"-n", "-i", "NONE", "--headless", "-u", "NONE", "-l", script.u8string()};
        return out;
    }
#ifdef _WIN32
    if (lower(file.extension().u8string()) != ".exe") {
        out.error = file.u8string() + " is no program (.exe) and no bin/ folder of a " + name +
                    ".nvim checkout; it would need a shell";
        return out;
    }
#endif
    out.program = file;
    return out;
}

}  // namespace

TasksCommand find_tasks() {
    return find_nvim_command("tasks_cli", "tasks");
}

TasksCommand find_testing() {
    return find_nvim_command("testing_cli", "testing");
}

fs::path find_claude(std::string* error) {
    fs::path file = setting_path("claude_cli");
    std::error_code ec;
    if (file.empty()) {
#ifdef _WIN32
        file = process::find_on_path({"claude.exe"});
#else
        file = process::find_on_path({"claude"});
#endif
        if (file.empty()) {
            if (error) *error = "Claude Code (claude) is not on PATH (set claude_cli in the settings)";
            return {};
        }
        return file;
    }
    if (!fs::is_regular_file(file, ec)) {
        if (error) *error = "claude_cli names no file: " + file.u8string();
        return {};
    }
#ifdef _WIN32
    if (lower(file.extension().u8string()) != ".exe") {
        if (error) *error = "claude_cli must name a program (.exe), not a script that needs a shell: " + file.u8string();
        return {};
    }
#endif
    return file;
}

config::Status parse_brief(const std::string& text, Brief& out) {
    const json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return {422, "the tasks command gave no JSON brief (an older tasks.nvim without agent-brief?)"};
    }
    if (doc.value("schema", "") != "tasks-agent-brief/1") {
        return {422, "the brief is not tasks-agent-brief/1 but '" + doc.value("schema", std::string("?")) + "'"};
    }
    if (!doc.contains("brief_version") || !doc["brief_version"].is_number_integer() ||
        !doc.contains("contract_schema") || !doc["contract_schema"].is_number_integer() ||
        !doc.contains("markdown") || !doc["markdown"].is_string()) {
        return {422, "the brief lacks brief_version, contract_schema or markdown"};
    }
    Brief brief;
    brief.brief_version = doc["brief_version"].get<int>();
    brief.contract_schema = doc["contract_schema"].get<int>();
    brief.engine_version = doc.value("engine_version", "");
    brief.markdown = doc["markdown"].get<std::string>();
    if (doc.contains("checks") && doc["checks"].is_object() && doc["checks"].contains("source") &&
        doc["checks"]["source"].is_string()) {
        // Only for the job's info: what the checks are is in the Markdown; the allowed tools never follow them.
        brief.checks_source = jobs::sanitize(doc["checks"]["source"].get<std::string>()).substr(0, 32);
    }
    if (brief.brief_version < kBriefVersion || brief.contract_schema < kContractSchema) {
        return {422, "the tasks engine is older than ai-desktop expects (brief " + std::to_string(brief.brief_version) +
                         ", contract " + std::to_string(brief.contract_schema) + "; needs " +
                         std::to_string(kBriefVersion) + " and " + std::to_string(kContractSchema) + ")"};
    }
    if (trimmed(brief.markdown).empty()) {
        return {422, "the brief is empty"};
    }
    if (const std::string error = jobs::text_error(brief.markdown, true); !error.empty()) {
        return {422, "the brief " + error};
    }
    if (brief.brief_version > kBriefVersion || brief.contract_schema > kContractSchema) {
        brief.note = "the brief is newer than this ai-desktop (brief " + std::to_string(brief.brief_version) +
                     ", contract " + std::to_string(brief.contract_schema) + "); used as it is";
    }
    out = std::move(brief);
    return {};
}

config::Status fetch_brief(const TasksCommand& tasks, const std::string& by, Brief& out, const fs::path& repo) {
    if (!tasks.ok()) {
        return {422, tasks.error};
    }
    process::Spec spec;
    spec.program = tasks.program;
    spec.args = tasks.prefix;
    spec.args.insert(spec.args.end(), {"agent-brief", "--format=json", "--by=" + by});
    if (!repo.empty()) {
        spec.args.push_back("--repo=" + repo.u8string());
    }
    std::string text;
    std::string error;
    const int code = process::run_capture(spec, std::chrono::seconds(30), kMaxBriefBytes, text, &error);
    if (code != 0) {
        return {422, "tasks agent-brief failed (" + std::to_string(code) + "): " + one_line(trimmed(error))};
    }
    return parse_brief(text, out);
}

fs::path work_dir(const std::string& vault, const std::string& area, std::string* error) {
    const fs::path root = fs::u8path(vault);
    std::error_code ec;
    if (!root.is_absolute() || !fs::is_directory(root, ec)) {
        if (error) *error = "the vault folder " + vault + " does not exist here";
        return {};
    }
    const fs::path repo = root.parent_path() / fs::u8path(area);
    if (repo != root && fs::exists(repo / ".git", ec)) {
        return repo;
    }
    return root;
}

const std::vector<std::string>& allowed_tools() {
    // The checks of the CI rhythm (brief 2, `checks`): `ci commit <sha> --quick` per commit and `ci commit HEAD
    // --base=origin/main` before the push take arguments; `ci land` takes none. Not `ci:*`: no other ci command.
    static const std::vector<std::string> tools = {"Bash(tasks:*)",
                                                   "Bash(git:*)",
                                                   "Bash(scripts/test.sh ci commit:*)",
                                                   "Bash(testing ci commit:*)",
                                                   "Bash(scripts/test.sh ci land)",
                                                   "Bash(testing ci land)",
                                                   "Read",
                                                   "Edit",
                                                   "Write",
                                                   "Glob",
                                                   "Grep"};
    return tools;
}

const std::vector<std::string>& disallowed_tools() {
    static const std::vector<std::string> tools = {"Bash(git push --no-verify:*)", "Bash(git -c:*)"};
    return tools;
}

const std::string& tool_set() {
    static const std::string tools = "Bash,Read,Edit,Write,Glob,Grep";
    return tools;
}

std::vector<std::string> claude_args(const fs::path& brief_file, const fs::path& vault, const std::string& model) {
    // --tools is the set of built-in tools the session has at all (the allowed list only says which run without a
    // question): without it every other built-in tool (Task, Workflow, Monitor, EnterWorktree, CronCreate, Artifact,
    // SendMessage, ...) is there too. --strict-mcp-config: no MCP server of the person's configuration.
    std::vector<std::string> args = {"-p", "--output-format", "stream-json", "--verbose",
                                     "--append-system-prompt-file", brief_file.u8string(),
                                     "--permission-mode", "default", "--tools", tool_set(), "--strict-mcp-config",
                                     "--add-dir", vault.u8string()};
    if (!model.empty()) {
        args.insert(args.end(), {"--model", model});
    }
    // The refused commands before the allowed list; each list takes every word up to the next option.
    args.push_back("--disallowedTools");
    for (const auto& tool : disallowed_tools()) {
        args.push_back(tool);
    }
    // Last: the list of --allowedTools takes every word up to the next option.
    args.push_back("--allowedTools");
    for (const auto& tool : allowed_tools()) {
        args.push_back(tool);
    }
    return args;
}

std::vector<std::pair<std::string, std::string>> session_env_removals(const std::vector<std::string>& names) {
    std::vector<std::pair<std::string, std::string>> out;
    if (std::find(names.begin(), names.end(), "CLAUDECODE") == names.end()) {
        return out;
    }
    static const char* const kPrefixes[] = {"CLAUDECODE", "CLAUDE_CODE_", "CLAUDE_AGENT_SDK_", "CLAUDE_PREVIEW_"};
    static const char* const kNames[] = {"CLAUDE_PID", "CLAUDE_EFFORT", "ANTHROPIC_BASE_URL", "USE_LOCAL_OAUTH",
                                         "USE_STAGING_OAUTH"};
    for (const auto& name : names) {
        bool drop = std::find(std::begin(kNames), std::end(kNames), name) != std::end(kNames);
        for (const char* prefix : kPrefixes) {
            drop = drop || name.rfind(prefix, 0) == 0;
        }
        if (drop) {
            out.emplace_back(name, "");
        }
    }
    return out;
}

std::string prompt(const jobs::Request& request, const std::string& by, const std::string& branch) {
    std::string text = "Work on the task " + request.task_id + " of the task vault " + request.vault + " (area " +
                       request.area + "), handed over by " + request.source + " (kind " + request.kind + "). ";
    if (request.kind != "handoff") {
        text += "This kind has no instructions of its own yet: treat it as a hand-over. ";
    }
    if (!branch.empty()) {
        text += "You work in a git worktree of your own (the current folder, branch " + branch +
                ", made from origin/main), not in the shared checkout of the repository, where other sessions keep "
                "uncommitted work: change and commit only here. Check and deliver as the checks in your system prompt say "
                "(without them: `git pull --rebase`, then `git push origin HEAD:main`). When the run ends the worktree is removed: uncommitted changes go with "
                "it, commits that are on no remote stay on the branch " + branch + ". ";
    }
    text += "Follow the workflow of the tasks family in your system prompt with the name " + by +
            ": first `tasks claim " + request.task_id + " --by=" + by +
            "` (a claim of somebody else: stop and say so), then do what its acceptance asks, and at the end `tasks done " +
            request.task_id + " ... --by=" + by + "` (or give it back with `tasks unclaim " + request.task_id +
            " --by=" + by + "` and say why). The task as it is now:\n\n" + request.markdown;
    return text;
}

namespace {

// render() for one parsed line; json::value() and get() throw on a value of another type than expected.
void render_doc(const json& doc, Event& event) {
    const std::string type = doc.value("type", "");
    if (type == "system" && doc.value("subtype", "") == "init") {
        event.text = "▸ Claude Code";
        if (doc.contains("model") && doc["model"].is_string()) {
            event.text += " (" + doc["model"].get<std::string>() + ")";
        }
        if (doc.contains("cwd") && doc["cwd"].is_string()) {
            event.text += " in " + doc["cwd"].get<std::string>();
        }
        event.text += "\n";
    } else if (type == "assistant" && doc.contains("message") && doc["message"].is_object()) {
        const json& content = doc["message"].value("content", json::array());
        for (const auto& part : content) {
            if (!part.is_object()) continue;
            const std::string kind = part.value("type", "");
            if (kind == "text" && part.contains("text") && part["text"].is_string()) {
                event.text += part["text"].get<std::string>() + "\n";
            } else if (kind == "tool_use") {
                std::string what;
                if (part.contains("input") && part["input"].is_object()) {
                    for (const char* key : {"command", "file_path", "pattern", "path"}) {
                        if (part["input"].contains(key) && part["input"][key].is_string()) {
                            what = part["input"][key].get<std::string>();
                            break;
                        }
                    }
                }
                event.text += "▸ " + part.value("name", std::string("tool")) + (what.empty() ? "" : ": " + one_line(what)) + "\n";
            }
        }
    } else if (type == "user" && doc.contains("message") && doc["message"].is_object()) {
        const json& content = doc["message"].value("content", json::array());
        if (content.is_array()) {
            for (const auto& part : content) {
                if (part.is_object() && part.value("type", "") == "tool_result" && part.value("is_error", false)) {
                    std::string detail;
                    if (part.contains("content") && part["content"].is_string()) {
                        detail = part["content"].get<std::string>();
                    } else if (part.contains("content") && part["content"].is_array() && !part["content"].empty() &&
                               part["content"][0].is_object() && part["content"][0].contains("text") &&
                               part["content"][0]["text"].is_string()) {
                        detail = part["content"][0]["text"].get<std::string>();
                    }
                    event.text += "  ✗ " + one_line(detail) + "\n";
                }
            }
        }
    } else if (type == "result") {
        event.final = true;
        event.is_error = doc.value("is_error", false) || doc.value("subtype", "success") != "success";
        if (event.is_error) {
            event.error = doc.contains("result") && doc["result"].is_string() ? one_line(doc["result"].get<std::string>())
                                                                              : doc.value("subtype", std::string("error"));
        }
        if (doc.contains("usage") && doc["usage"].is_object()) {
            const json& usage = doc["usage"];
            if (usage.contains("input_tokens") && usage["input_tokens"].is_number_integer()) {
                event.input_tokens = usage["input_tokens"].get<long>();
            }
            if (usage.contains("output_tokens") && usage["output_tokens"].is_number_integer()) {
                event.output_tokens = usage["output_tokens"].get<long>();
            }
        }
    }
}

}  // namespace

Event render(const std::string& line) {
    Event event;
    const json doc = json::parse(line, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return event;
    }
    try {
        render_doc(doc, event);
    } catch (const json::exception&) {
        // A line of another form than Claude Code writes (a key of another type): skipped like one that is no JSON,
        // never an exception out of the reading of the run.
        event = Event{};
    }
    // Text from the agent is foreign text: the output keeps only what the character rule of the jobs allows.
    event.text = jobs::sanitize(event.text);
    event.error = jobs::sanitize(event.error);
    return event;
}

}  // namespace ai_desktop::agent
