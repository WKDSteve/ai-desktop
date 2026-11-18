# Jobs from the family apps

An app of the family (tasks-desktop with its button, tasks.nvim with `:Tasks agent`) hands a task to ai-desktop. The
task stands in the dashboard as a new job, either only filled in (`prefill`) or already running (`run`), and the
dashboard comes to the front. Only programs of the person on this machine can do that; a web page cannot.

## The contract

### `POST /jobs`

```
POST /jobs HTTP/1.1
Host: 127.0.0.1:<port>
Authorization: Bearer <the 64 hex digits of control.secret>
Content-Type: application/json

{
  "source":   "tasks-desktop",
  "task_id":  "ai-desktop/auftrag-von-aussen-annehmen",
  "title":    "Auftrag von außen annehmen",
  "markdown": "---\ntitle: ...\n---\n\n## Akzeptanz\n...",
  "vault":    "E:/repos/tasks-vault",
  "area":     "ai-desktop",
  "mode":     "prefill",
  "kind":     "handoff",
  "model":    "llama3.1:8b",
  "private":  false
}
```

| Field | Required | Rule |
|---|---|---|
| `source` | yes | the app, `[a-z0-9._-]`, up to 64 bytes |
| `task_id` | yes | `<area>/<slug>`, `[A-Za-z0-9._/-]`, up to 200 bytes; no `.` or `..` segment, no leading, trailing or double `/` |
| `title` | yes | one line, not empty, up to 300 bytes |
| `markdown` | yes | the task file, up to 256 KiB |
| `vault` | yes | the folder of the vault, one line, up to 1024 bytes (shown and passed on, never opened by ai-desktop) |
| `area` | yes | `[A-Za-z0-9._-]`, up to 100 bytes; empty for a `draft-task` whose area is not chosen yet |
| `mode` | yes | `prefill` (stands in the dashboard, nothing runs) or `run` (runs at once) |
| `kind` | no | `handoff` (default), `review-done`, `draft-task`, `prepare-decision`, `split-plan`; see below |
| `model` | no | the model of a run: `claude-code` (or `claude-code:<model>`) runs Claude Code with the brief of the tasks family, any other model answers once (see "Runs"); empty: the setting `jobs_model` (`claude-code` unless set) |
| `private` | no | `true` when the caller knows the area is private (see "Private areas") |
| `choices` | `draft-task` only | the values the vault allows: `{areas, kinds, tags?, categories?, plans?}`, see "Drafts"; refused on any other kind |

Every text is valid UTF-8 and holds no control or invisible character, the same rule as
`tools/check-control-chars.py`: no C0 control (in `markdown` line feed, carriage return and tab are allowed), no DEL,
no C1, no soft hyphen, no zero-width, line or paragraph separator, bidirectional control or byte order mark. An unknown
field is refused. The whole body is at most 320 KiB.

`kind` names what the job asks for, so the family has one channel: `handoff` hands over a task to work on,
`review-done` checks a finished task against its acceptance (see "Reviews"), `prepare-decision` prepares a task that
waits for a decision (see "Decisions"), `split-plan` splits a plan into task drafts (see "Splitting a plan"),
`draft-task` turns a free text into the fields of a new task (see "Drafts").

Answer `201`:

```json
{ "job_id": "3f9c0a1b2c3d4e5f", "status": "prefilled", "private": false }
```

`status` is `prefilled`, `running` (mode `run`) or `needs_confirmation` (mode `run` for a private area on a model
that is not local; the job waits in the dashboard, `note` says why). A `run` while two jobs run already stays
`prefilled`, with a `note`.

| Status | When |
|---|---|
| `401` | no `Authorization: Bearer <secret>`, or a wrong one |
| `403` | a foreign `Host`; an `Origin`, `Sec-Fetch-Site` or `Sec-Fetch-Mode` header (a browser sent it); the setting `jobs` is `off` |
| `413` | the body is over 320 KiB |
| `415` | `Content-Type` is not `application/json` |
| `429` | more than 10 jobs in a minute (`Retry-After` in seconds; `AI_DESKTOP_JOBS_PER_MINUTE` sets 1-1000) |
| `400` | a field is missing, of the wrong type, too long or holds a refused character |
| `503` | ai-desktop could not write its control secret at start, so it takes no job |

### The routes of the dashboard

| Route | Who | What |
|---|---|---|
| `GET /jobs` | any local caller | the jobs, newest last, without their text; `enabled` = the setting `jobs` |
| `GET /jobs/<id>` | any local caller | one job with `markdown` and `output` |
| `POST /jobs/<id>/run` | the dashboard (its `Origin`) or the control secret | `{ "model"?, "markdown"?, "confirm_private"? }`: run it with the text the person left; `409` with `"status": "needs_confirmation"` for a private area on a model that is not local without `confirm_private: true` |
| `POST /jobs/<id>/cancel` | likewise | stop a running job: the process tree of an agent ends, a single answer is no longer shown; `409` when it does not run |
| `DELETE /jobs/<id>` | likewise | put it away (`204`; a running job is cancelled first) |

All of them refuse a foreign `Host` (`403`). The jobs live in memory only: a task can be private, so nothing of it is
written to disk, and the jobs are gone when the server ends (at most 100; the oldest that does not run goes first).
A run is metadata in the usage log like every `/ask/stream` (never its text); a run of Claude Code is logged with
the provider `claude-code` and the tokens of its result.

A job shows how it ran: `runner` (`claude-code` or `model`), `brief` (`brief_version`, `contract_schema`,
`engine_version` of the brief an agent got), `note` (why it fell back to one answer, or that the brief is newer than
this ai-desktop) and the statuses `running`, `done`, `failed`, `cancelled`.

## Runs

**Claude Code (`claude-code`).** A job of the tasks family is worked on by Claude Code headless, started by
ai-desktop (`src/agent.cpp`, `src/process.cpp`):

1. The brief: `tasks agent-brief --format=json --by=ai-desktop:<job id>` (tasks.nvim, `docs/COMMANDS.md` there). Its
   `schema` must be `tasks-agent-brief/1`, `brief_version` and `contract_schema` at least 1: an older engine is
   recognised and refused (the job then answers once and says so); a newer one is used and named in `note`.
2. The tasks command: the setting `tasks_cli` (an absolute path), else `tasks` on `PATH`. The `bin/tasks` or
   `bin/tasks.cmd` of a tasks.nvim checkout runs as `nvim -n -i NONE --headless -u NONE -l <checkout>/scripts/tasks.lua`
   (no `cmd.exe` or `sh` in between); any other file must be a program (`.exe` on Windows).
3. Claude Code: the setting `claude_cli`, else `claude` on `PATH` (`claude.exe` on Windows; a `.cmd` would need a
   shell and is refused).
4. The folder: the repository of the area beside the vault (`<vault>/../<area>` with a `.git`, the layout of
   `$REPOS_DIR`), else the vault; the vault is always added (`--add-dir`) and named in `TASKS_VAULT`.
5. The call: `claude -p --output-format stream-json --verbose --append-system-prompt-file <brief>
   --permission-mode default --tools Bash,Read,Edit,Write,Glob,Grep --strict-mcp-config --add-dir <vault>
   [--model <m>] --allowedTools "Bash(tasks:*)" "Bash(git:*)" Read Edit Write Glob Grep`, the prompt (the task, its
   id, the claim name `ai-desktop:<job id>`, what to do: claim, work, done or unclaim) on stdin. The folder of the tasks command comes first on the agent's `PATH`. No
   `--dangerously-skip-permissions`, no `bypassPermissions`: in print mode a tool outside the list is refused, and the
   person's own settings of Claude Code (`~/.claude`) apply as in any session of theirs. `--tools` is the set of
   built-in tools the session has at all: without it Claude Code also brings Task, Workflow, Monitor, EnterWorktree,
   CronCreate, Artifact, SendMessage and more, which the allowed list does not name; `--strict-mcp-config` leaves out
   the MCP servers of the person's configuration.
6. The output: every line of stream-json becomes text in the job (the agent's words, `▸ Bash: tasks claim ...` for a
   tool, `✗ ...` for a refused one), cleaned by the character rule above; the `result` line ends it.

Without Claude Code, the tasks command, its brief or the vault folder, the job answers once with `default_model` and
`note` says why; a private job whose fallback model is not local sends nothing and fails. When ai-desktop itself runs
inside a Claude Code session (`CLAUDECODE` is set), that session's variables (`CLAUDE_CODE_*`, `CLAUDE_AGENT_SDK_*`,
`ANTHROPIC_BASE_URL`, ...) are not passed on, so the agent signs in as the person's own `claude` does.

**One answer (any other model).** The task goes to the model through the ModelRouter, as `/ask/stream`; no tools.

## Reviews (`kind: review-done`)

A finished task is checked against its acceptance (`src/review.cpp`). The caller sends the task as it was finished
(its Backlog file, with `commits` and `done_in`); the agent **only reads and answers**, nothing is written by it:

1. ai-desktop reads the `commits:` of the front matter (`[repo@sha, ...]` or a block list, at most 20) and runs
   `git -C <vault>/../<repo> show --stat --patch --no-color --no-ext-diff --no-textconv <sha>` for each (20 s, 48 KiB
   a diff, 160 KiB together, cut and said so). A repository that is not beside the vault, an unknown commit or a
   missing git is named in the prompt instead of a diff.
2. The prompt: the task, the diffs, and the instruction to decide for every line of `## Akzeptanz`/`## Acceptance`
   whether it is met, open or not checkable, with evidence (`path:line` or `repo@sha`), and to end the answer with one
   block from `<!-- review:begin -->` to `<!-- review:end -->`: a section `## Prüfung` (`## Review` in English), one
   line per criterion, and, when something is open, `### Folgetask (Vorschlag)` with `- title:` and `- summary:`.
3. On `claude-code`: the brief is not needed (a review claims nothing); `--tools Read,Glob,Grep` (the only built-in
   tools of the session) and `--strict-mcp-config` (no MCP server), `--allowedTools Read Glob Grep`,
   `--disallowedTools Bash Edit Write MultiEdit NotebookEdit WebFetch WebSearch` (a denial wins over the person's own
   settings of Claude Code, so the run stays read-only whatever `~/.claude` allows), the vault and the repositories of
   the commits as `--add-dir`, the working folder as for a hand-over. Another model answers once with the same prompt.
4. The block is a proposal. The caller shows it and adds it to the task as a section only after a yes (tasks.nvim:
   `:Tasks agent <id> kind=review-done`); a proposed follow-up task is created only after a second yes.

## Decisions (`kind: prepare-decision`)

A task that waits for the person (`status: decision`) is prepared, the same read-only way as a review: the diffs of
the commits its `refs:` name (`repo@sha`; paths in `refs` are left to the agent to read), read access to the vault, the
repository of the area and those of the references, the same tools allowed and refused. The prompt asks for 2 to 4
real options with their consequences, one recommendation with its reason and the assumptions nobody confirmed, and for
one block from `<!-- decision:begin -->` to `<!-- decision:end -->` with two sections:

- `## Vorschlag` (`## Proposal`): `### Optionen`, `### Empfehlung`, `### Offene Annahmen`;
- `## Offene Fragen` (`## Open questions`): one `### ...? [qN]` per decision, its options as unticked boxes, exactly
  one marked `(empfohlen)`: the format of tasks.nvim's questions, which `tasks answer` (and the question view of
  tasks-desktop) answers with one choice.

The agent decides nothing. The caller adds the sections only after a yes, and the task stays `decision`.

## Splitting a plan (`kind: split-plan`)

The caller sends the plan file as `markdown` (its id as `task_id`), with the list of the plan's tasks appended (id,
status, phase, title), so that a model without tools sees them too. The run is read-only as a review is (the vault and
the repository of the area to read); no commits are read, but ai-desktop puts the brief of the tasks family
(`tasks agent-brief`, the allowed values of every field) into the prompt when the tasks command is there, else the
values it knows. The prompt asks for what is missing to reach the plan's definition of done: no draft that repeats a
task of the plan, small checkable tasks, the plan's phases where they fit. The answer ends with one JSON object between
`<!-- split:begin -->` and `<!-- split:end -->`:

```json
{"phases": [{"name": "build", "new": false}],
 "tasks": [{"title": "...", "area": "...", "kind": "feature", "prio": 2, "effort": "S", "value": 3,
            "phase": "build", "summary": "...", "acceptance": ["..."], "after": ["area/slug", "#1"],
            "blocked_by": []}]}
```

`#N` in `after`/`blocked_by` names another draft of the list. Nothing is created by the agent: the caller shows the
drafts to tick and creates the ticked ones after a yes, a new phase only after its own question.

Why read-only and no note file: the result must never reach the task by itself, and a single place for it (the job's
output) is what the caller reads and offers. A private area asks first, as every job does.

## Drafts (`kind: draft-task`)

A free text (a note, a piece of a mail, a thought) becomes the fields of a new task (`src/draft.cpp`). The caller
(tasks-desktop's "Felder vorschlagen", tasks.nvim's `:Tasks new` with a text) sends the text as `markdown`, any
`task_id` (`draft/new`), its form's title or the start of the text as `title`, `area` empty or the area chosen so far,
and the values its vault allows:

```json
"choices": {
  "areas": ["ai-desktop", "tasks.nvim"],
  "kinds": ["task", "feature", "bug"],
  "tags": ["ui", "tasks-family"],
  "categories": ["ux"],
  "plans": ["ALL/ki-der-ai-familie-in-der-tasks-familie-tasks-an-einen-agente"]
}
```

`areas` and `kinds` must not be empty; every entry is a name of `[A-Za-z0-9._/-]` (1-200 bytes, at most 1000 a list).

1. **Private, always.** The area of a draft is not known yet, so the strictest class holds whatever `area` and
   `private` say: a local model runs at once, any other (Claude Code included) waits for a yes (`needs_confirmation`;
   the caller asks its person and sends `POST /jobs/<id>/run` with `confirm_private: true` and the secret, or the
   person answers in the dashboard). The dashboard does not come to the front: the answer goes to the form that asked.
2. **One answer, no tools.** On `claude-code` the call is `claude -p --output-format stream-json --verbose
   --append-system-prompt <system> --permission-mode default --tools "" --strict-mcp-config [--model <m>]
   --disallowedTools Bash Edit Write MultiEdit NotebookEdit WebFetch WebSearch Read Glob Grep Task TodoWrite` (no
   built-in tool and no MCP server at all; the refused list on top), in an empty folder of its own
   (`<config>/draft-work`, so no `CLAUDE.md` of a project is read), no `--add-dir`. Without Claude Code the job answers
   with `default_model`, and only when that is local. Any other model answers once.
3. **The prompt** lists the allowed values, asks for title, summary and acceptance in the language of the text, and
   for one block from `<!-- draft:begin -->` to `<!-- draft:end -->` holding one JSON object (a ```` ```json ```` fence
   is allowed). The text stands in a fence of its own, as data.
4. **The check.** When the run ends, ai-desktop reads the last block and checks it; `GET /jobs/<id>` then has either
   `draft` or `draft_error` (the run itself is `done` either way; `failed` only when the model gave no answer):

```json
"draft": {
  "schema": "tasks-draft/1",
  "title": "Knopf blau machen",
  "area": "tasks.nvim", "kind": "task", "prio": 2, "effort": "S", "value": 3,
  "tags": ["ui"], "category": ["ux"], "plan": "ALL/...",
  "summary": "Der Knopf wird blau.",
  "acceptance": ["Der Knopf ist blau"],
  "dropped": ["tags: made-up"]
}
```

| Key | Rule |
|---|---|
| `title` | required, one line, 1-300 bytes after trimming |
| `area`, `kind`, `plan` | one of `choices`; another value is left out and named in `dropped`; `""` means none |
| `tags`, `category` | lists (a single string is taken as a list of one); the allowed ones are kept (at most 20), the others dropped |
| `prio` | 1-3, `value` 1-5 (a digit as a string is taken); out of range: dropped |
| `effort` | `XS`, `S`, `M`, `L`, `XL`; another: dropped |
| `summary` | text up to 2000 bytes (lines allowed) |
| `acceptance` | up to 20 lines of at most 500 bytes; a leading `- [ ] ` is taken off, empty lines are left out |

A key that is missing stays missing (the form keeps its default). A value of the wrong type, a missing or empty
title, or a control character (the character rule above) makes the whole draft invalid: `draft_error` names why and
nothing is filled in. The caller checks the values once more against its own lists (they may have changed), fills its
form, and the person creates the task with the form's own button: ai-desktop never writes a task.

## The control secret

At every start ai-desktop makes 32 random bytes (the random source of the system: `BCryptGenRandom`, `/dev/urandom`)
and writes them as 64 lower-case hex digits and a line feed to `control.secret` in its config folder
(the folder of `hub.json`: `AI_DESKTOP_CONFIG_DIR`, else `%APPDATA%\ai-desktop`, `~/.config/ai-desktop`,
`~/Library/Application Support/ai-desktop`; never the old loomAI folder that `config.json` may still be read from),
the file and the form of `control.secret` of the appkit hubs (`appkit-core/src/registry.rs`):

- An old file is removed first, and the secret goes into a new file that has its rights from its first moment: a
  program that opened the old one (or planted it) keeps a handle on that file only, never on the secret.
- Windows: the rights are the current user alone (one entry, full control, nothing inherited: the folder's inherited
  list can name other groups), given when the file is created. If they cannot be given or the old file cannot be
  removed, no secret is written and `POST /jobs` answers `503`; the server runs on.
- Elsewhere: mode `0600`, the file made with `O_EXCL` (never through a link).
- It is written once the port is bound, like `hub.json`: a second start whose port is taken leaves the secret of the
  running server alone.
- The file is removed when the server ends (also on Ctrl+C, closing its console, SIGINT, SIGTERM), together with
  `hub.json`; after a crash the next start replaces it. The compare is constant in time.

A family app reads the file each time it sends (the secret changes with every start) and sends
`Authorization: Bearer <hex>`. Where ai-desktop listens, and its pid for the focus right below, come from `hub.json`
in the same folder (README, "Found by the other apps").

## Bringing the window to the front

Windows does not let a process in the background take the focus (the foreground lock). The app that hands over the
job is in the foreground (the person just clicked or typed), so it grants the right before it sends:
`AllowSetForegroundWindow(<pid of ai-desktop>)`, the pid from the discovery file. ai-desktop then

1. brings a window whose title starts with `ai Dashboard` / `KI Dashboard` (the dashboard tab is open and shown) to
   the front, restoring it when it is minimised; the page selects the new job from the event it gets;
2. else opens `http://127.0.0.1:<port>/#job=<id>` in the default browser, passing its right on
   (`AllowSetForegroundWindow(ASFW_ANY)`), and the page selects that job.

Without the right Windows flashes the task bar button instead, which is the right thing for a request nobody gave. On
Linux the page is opened with `xdg-open` (only with a display), on macOS with `open`. `AI_DESKTOP_JOBS_FOREGROUND=0`
turns all of it off (the tests do).

## Private areas

The setting `private_areas` (comma-separated area names, `AI_DESKTOP_PRIVATE_AREAS` locks it; matched without regard
to case, since an area is a folder and `Kunde` and `kunde` are the same one on Windows) names the areas whose
tasks go to a local model only (Ollama, or a custom model on a loopback address). A request may also say `"private":
true`. A private job that is to run on any other model is held (`needs_confirmation`); the dashboard asks
"The area ... is private. The model ... does not run locally: shall the task go there anyway?", and only the answer
yes sends it (`confirm_private`). A web page cannot answer the question: the run route wants the dashboard's `Origin`
or the secret, and a page cannot send that `Origin`. A local program can: it sets the header itself (see the threat
model), so the question protects against pages and mistakes, not against a program that means harm.

## Settings

| Key | Env | Default | Meaning |
|---|---|---|---|
| `jobs` | `AI_DESKTOP_JOBS` | `on` | `off` refuses every `POST /jobs` (403); the jobs already there can still be run |
| `private_areas` | `AI_DESKTOP_PRIVATE_AREAS` | (none) | see above |
| `jobs_model` | `AI_DESKTOP_JOBS_MODEL` | `claude-code` | the model of a job that names none |
| `claude_cli` | `AI_DESKTOP_CLAUDE_CLI` | (PATH) | absolute path of Claude Code |
| `tasks_cli` | `AI_DESKTOP_TASKS_CLI` | (PATH) | absolute path of the tasks command (`bin/tasks` of tasks.nvim) |

The first two are in the dashboard under "Settings of the jobs"; all are in `GET/PUT /settings`.

## Threat model

**What is protected:** the person's tasks (they can be private: a private area, a customer), the models and keys of
ai-desktop (a run costs money and sends text to a provider), and the person's attention (a job brings a window to the
front).

**Trust boundary:** the user account on this machine. Every program that runs as the person can read their files,
the control secret included; that is the same boundary as the appkit hubs, and a program with those rights could as
well read the vault itself. The secret guards `POST /jobs` only. The rest of the management API (run, cancel and
delete a job, `PUT /settings`) is guarded against web pages only (`Host`, `Origin`, `Sec-Fetch-*`), not against
programs: the dashboard proves itself by its `Origin`, and any program that is not a browser can send that header
itself. So everything that reaches `127.0.0.1` (every program of every user account on this machine) can use those
routes; the boundary holds there only on a machine with one account. A login for the whole management API is a task
of its own (ai-desktop, "Anmeldung für die ganze Verwaltungs-API").

| Attacker | Attack | Defence |
|---|---|---|
| A web page in the person's browser | `fetch`/form `POST /jobs` to 127.0.0.1 (CSRF) | a browser always sends `Origin` on a POST (and `Sec-Fetch-*`): refused with 403 before anything else, the dashboard's own origin included. The page cannot read `control.secret`, so it has no secret; `application/json` makes a cross-origin request a preflight, which gets no CORS headers (no `Access-Control-Allow-*` anywhere). |
| A web page through DNS rebinding | a name of its own resolved to 127.0.0.1, so the browser treats it as same-origin and sends no `Origin` on a GET | every request of the server (the pre-routing handler) must name `127.0.0.1`, `localhost` or `[::1]` with the port in `Host`: a foreign name gets 403, so the page reads no job and no setting. The routes of the jobs also refuse a request without `Host`. |
| Another machine | reach the port | the server binds `127.0.0.1` only. |
| Another user on this machine | read the secret, send jobs | the file is readable by the person only (Windows: protected ACL with one entry; elsewhere 0600), so `POST /jobs` is closed. **Not defended:** loopback is shared by all accounts, and the other routes take a forged `Origin` (next row). |
| A local program without the secret (the person's or another account's) | send a job, confirm a private area, put jobs away, change the settings | `401` for `POST /jobs`. **Not defended otherwise:** run, cancel and delete want the dashboard's `Origin` or the secret, and a program sends `Origin: http://127.0.0.1:<port>` itself; `GET/PUT /settings` want no proof at all. So it can run any job, send a private one to a cloud model with `confirm_private: true` without the person's yes, stop or delete jobs, and set `claude_cli` to a program of its own, which the next run then starts **as the person**. It can read jobs (`GET /jobs`). For a program of the person this gives nothing new; for another account it is a way in. |
| A family app gone wrong | flood the dashboard, send huge or hostile text | 10 jobs a minute (counted after the secret, so a stranger cannot use them up), 320 KiB a body, 100 jobs kept, 2 runs at once; every text follows the character rule above (no bidi trick, no escape sequence for a terminal); the page shows the text as text only (Trusted Types, `textContent`/`value`). |
| A stolen old secret | replay it | a new secret at every start; the file is removed at the end. |
| A cloud provider | gets a private task | only after the person said yes in the dashboard; a local model needs no question. |
| A review that writes | the review agent changes the task or the code it checks | the session has only Read, Glob and Grep (`--tools`) and no MCP server (`--strict-mcp-config`), the writing ones are refused explicitly as well (`--disallowedTools`), no shell; ai-desktop runs only `git show` itself; the result goes to the task only after the person's yes. |
| Focus theft | a job steals the focus at a bad moment | ai-desktop never takes the focus on its own: only with the right the sending app granted at the person's click. |
| The agent (Claude Code) | it writes in the vault and the repository of the area, runs `tasks` and `git` (commit and push as the person) | it runs as the person, in the folders of the task (the file tools; see below for git), with the tools of its list (the shell tool only for `tasks` and `git`, no web tool, no MCP server; print mode refuses the rest) and the person's own Claude Code settings. **This is no sandbox:** `git` with `Write` can start any program as the person, e.g. a hook it writes into `.git/hooks` and then triggers with `git commit`, `git -c core.hooksPath=...`, `git -c core.sshCommand=...` or an alias `!...` it sets with `git config`, and git reaches any remote it is told to; so whatever such a program can do (network, files outside the task's folders), a steered agent can do too; it claims under its own name (`ai-desktop:<job>`), so a task it holds shows who; a claim of somebody else stops it (the brief says so). A run is started by a family app with the secret or by the person in the dashboard, never by a page. Stop ends it with its whole process tree (a job object on Windows, a process group elsewhere), and so does the end of ai-desktop. |
| A task steering the agent (prompt injection) | text in a task file tells the agent to do something else | the tool list bounds what it can do directly, but not what a program started through git can do (row above): a steered agent can in effect run any program as the person. What it did through its tools is in the job's output, and its commits are in git; what such a program did is not necessarily. A task from a vault the person does not trust must not be handed over. |
| The settings `claude_cli` / `tasks_cli` | a program sets them to another executable | only an absolute path, set through `PUT /settings`, which a page cannot reach (Origin, Host). **Not defended against another account:** `PUT /settings` asks no proof, so a program of another user can set them and have its executable started as the person (row "A local program" above). |

**Not covered:** a program running as the person (it has the secret, the vault and the person's Claude Code
anyway); another user account on this machine, as far as the routes beyond `POST /jobs` go (see the trust boundary);
an agent doing harm with its tools (it can change and push the repository of the area, and through git start any
program as the person): what it committed is in git and can be reverted; the real boundary is the person's account,
not the tool list.

**The agent may push (decision of 2026-11-18).** `Bash(git:*)` includes `git push`, on purpose: a task is finished
when its commits are pushed, and a commit that stays local goes unnoticed. The barriers are the repository's own:
its pre-push hooks (a repository that is pushed only by hand refuses an unconfirmed push) and its CI, which judges
every push. The hooks hold against a mistake, not against a steered agent: nothing enforces that it leaves out
`--no-verify` (`Bash(git:*)` allows `git push --no-verify`; the person's Claude Code rules only ask it not to), and
it can change the hooks themselves. The CI on the remote is the barrier the agent cannot reach. Forbidding it would be one
entry in the tool list (`--disallowedTools "Bash(git push:*)"`), should a repository ever need that.
