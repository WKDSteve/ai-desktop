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
| `model` | no | the model of a run; empty: the setting `default_model` |
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
| `DELETE /jobs/<id>` | likewise | put it away (`204`) |

All of them refuse a foreign `Host` (`403`). The jobs live in memory only: a task can be private, so nothing of it is
written to disk, and the jobs are gone when the server ends (at most 100; the oldest that does not run goes first).
A run is metadata in the usage log like every `/ask/stream` (never its text).

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

Both are in the dashboard under "Settings of the jobs" and in `GET/PUT /settings`.

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

**Not covered:** a program running as the person (it has the secret and the vault anyway); the content of a task
steering the model (prompt injection): a run of this version has no tools, it only answers; the agents with tools
that come later need their own rules.
