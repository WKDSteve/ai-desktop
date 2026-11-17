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
| `area` | yes | `[A-Za-z0-9._-]`, up to 100 bytes |
| `mode` | yes | `prefill` (stands in the dashboard, nothing runs) or `run` (runs at once) |
| `kind` | no | `handoff` (default), `review-done`, `draft-task`, `prepare-decision`, `split-plan`; see below |
| `model` | no | the model of a run: `claude-code` (or `claude-code:<model>`) runs Claude Code with the brief of the tasks family, any other model answers once (see "Runs"); empty: the setting `jobs_model` (`claude-code` unless set) |
| `private` | no | `true` when the caller knows the area is private (see "Private areas") |

Every text is valid UTF-8 and holds no control or invisible character, the same rule as
`tools/check-control-chars.py`: no C0 control (in `markdown` line feed, carriage return and tab are allowed), no DEL,
no C1, no soft hyphen, no zero-width, line or paragraph separator, bidirectional control or byte order mark. An unknown
field is refused. The whole body is at most 320 KiB.

`kind` names what the job asks for, so the family has one channel: `handoff` hands over a task to work on. The other
kinds are named and stored now and shown in the dashboard; their own prompts come with their own tasks, until then a
job of any kind runs with the prompt of a hand-over.

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
   --permission-mode default --add-dir <vault> [--model <m>] --allowedTools "Bash(tasks:*)" "Bash(git:*)" Read Edit
   Write Glob Grep`, the prompt (the task, its id, the claim name `ai-desktop:<job id>`, what to do: claim, work,
   done or unclaim) on stdin. The folder of the tasks command comes first on the agent's `PATH`. No
   `--dangerously-skip-permissions`, no `bypassPermissions`: in print mode a tool outside the list is refused, and the
   person's own settings of Claude Code (`~/.claude`) apply as in any session of theirs.
6. The output: every line of stream-json becomes text in the job (the agent's words, `▸ Bash: tasks claim ...` for a
   tool, `✗ ...` for a refused one), cleaned by the character rule above; the `result` line ends it.

Without Claude Code, the tasks command, its brief or the vault folder, the job answers once with `default_model` and
`note` says why; a private job whose fallback model is not local sends nothing and fails. When ai-desktop itself runs
inside a Claude Code session (`CLAUDECODE` is set), that session's variables (`CLAUDE_CODE_*`, `CLAUDE_AGENT_SDK_*`,
`ANTHROPIC_BASE_URL`, ...) are not passed on, so the agent signs in as the person's own `claude` does.

**One answer (any other model).** The task goes to the model through the ModelRouter, as `/ask/stream`; no tools.

## The control secret

At every start ai-desktop makes 32 random bytes (the random source of the system: `BCryptGenRandom`, `/dev/urandom`)
and writes them as 64 lower-case hex digits and a line feed to `control.secret` in its config folder
(the folder of `hub.json`: `AI_DESKTOP_CONFIG_DIR`, else `%APPDATA%\ai-desktop`, `~/.config/ai-desktop`,
`~/Library/Application Support/ai-desktop`; never the old loomAI folder that `config.json` may still be read from),
the file and the form of `control.secret` of the appkit hubs (`appkit-core/src/registry.rs`):

- Windows: the file is created empty, its rights are set to the current user alone (one entry, full control,
  nothing inherited: the folder's inherited list can name other groups), and only then the secret is written. If the
  rights cannot be set, no secret is written and `POST /jobs` answers `503`; the server runs on.
- Elsewhere: mode `0600`, also for an old file.
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

The setting `private_areas` (comma-separated area names, `AI_DESKTOP_PRIVATE_AREAS` locks it) names the areas whose
tasks go to a local model only (Ollama, or a custom model on a loopback address). A request may also say `"private":
true`. A private job that is to run on any other model is held (`needs_confirmation`); the dashboard asks
"The area ... is private. The model ... does not run locally: shall the task go there anyway?", and only the answer
yes sends it (`confirm_private`). The question cannot be answered blind by a program without the secret: the run route
wants the dashboard's `Origin` or the secret.

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
well read the vault itself. The secret keeps out everything outside that boundary.

| Attacker | Attack | Defence |
|---|---|---|
| A web page in the person's browser | `fetch`/form `POST /jobs` to 127.0.0.1 (CSRF) | a browser always sends `Origin` on a POST (and `Sec-Fetch-*`): refused with 403 before anything else, the dashboard's own origin included. The page cannot read `control.secret`, so it has no secret; `application/json` makes a cross-origin request a preflight, which gets no CORS headers (no `Access-Control-Allow-*` anywhere). |
| A web page through DNS rebinding | a name of its own resolved to 127.0.0.1, so the browser treats it as same-origin and sends no `Origin` on a GET | every request of the server (the pre-routing handler) must name `127.0.0.1`, `localhost` or `[::1]` with the port in `Host`: a foreign name gets 403, so the page reads no job and no setting. The routes of the jobs also refuse a request without `Host`. |
| Another machine | reach the port | the server binds `127.0.0.1` only. |
| Another user on this machine | read the secret, send jobs | the file is readable by the person only (Windows: protected ACL with one entry; elsewhere 0600). |
| A local program without the secret | send a job, confirm a private area, put jobs away | `401` for `POST /jobs`; run and delete want the dashboard's `Origin` or the secret. It can read jobs (`GET /jobs`), as it can read the vault. |
| A family app gone wrong | flood the dashboard, send huge or hostile text | 10 jobs a minute (counted after the secret, so a stranger cannot use them up), 320 KiB a body, 100 jobs kept, 2 runs at once; every text follows the character rule above (no bidi trick, no escape sequence for a terminal); the page shows the text as text only (Trusted Types, `textContent`/`value`). |
| A stolen old secret | replay it | a new secret at every start; the file is removed at the end. |
| A cloud provider | gets a private task | only after the person said yes in the dashboard; a local model needs no question. |
| Focus theft | a job steals the focus at a bad moment | ai-desktop never takes the focus on its own: only with the right the sending app granted at the person's click. |
| The agent (Claude Code) | it writes in the vault and the repository of the area, runs `tasks` and `git` (commit and push as the person) | it runs as the person, in the folders of the task only, with the tools of its list (no shell beyond `tasks` and `git`, no web access, no other program; print mode refuses the rest) and the person's own Claude Code settings; it claims under its own name (`ai-desktop:<job>`), so a task it holds shows who; a claim of somebody else stops it (the brief says so). A run is started by a family app with the secret or by the person in the dashboard, never by a page. Stop ends it with its whole process tree (a job object on Windows, a process group elsewhere), and so does the end of ai-desktop. |
| A task steering the agent (prompt injection) | text in a task file tells the agent to do something else | the tool list bounds what it can do at all (no network, no other program); what it did is in the job's output and in git. A task from a vault the person does not trust should not be handed over. |
| The settings `claude_cli` / `tasks_cli` | a program sets them to another executable | only an absolute path, set through `PUT /settings`, which a page cannot reach (Origin, Host); a program that can do that runs as the person anyway. |

**Not covered:** a program running as the person (it has the secret, the vault and the person's Claude Code
anyway); an agent doing harm within its tools (it can change and push the repository of the area): what it did is in
git and can be reverted, and its tool list is the boundary.
