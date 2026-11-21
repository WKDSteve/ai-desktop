# Programs ai-desktop starts

Only for a job of the family apps (`docs/JOBS.md`), and never a shell or a window. Everything else (the HTTP
server, the dashboard it serves, the calls to the model providers over HTTP) runs in ai-desktop's own process.

| What | When | How |
| --- | --- | --- |
| `claude` (Claude Code headless) and what it starts (its tools: `tasks`, `git`) | a job runs on `claude-code` | `src/process.cpp`: `CreateProcessW` with `CREATE_NO_WINDOW`, pipes for stdin/stdout/stderr and only those three handles inherited, in a job object with `KILL_ON_JOB_CLOSE`: Stop, the end of the run and the end of ai-desktop take the whole tree. Elsewhere `fork`/`execve` in a process group of its own. The run ends when `claude` ends, not when its output does: a watcher waits for the process and then ends the job object (`TerminateJobObject`) or the process group (`kill(-pid, SIGKILL)`, the child kept a zombie with `waitid(WNOWAIT)` until then), so a descendant that inherited stdout or stderr (a server or a hook's background job left behind) cannot hold the run on `running`; what `claude` wrote before it ended is still read from the pipe. |
| `nvim --headless ... scripts/tasks.lua agent-brief` (or the `tasks` program named in `tasks_cli`) | before such a run, for the brief | the same, ended after 30 s at most |
| `git -C <repo> fetch origin main`, `worktree add -b ai-desktop/job-<id>-<run> --track <dir> origin/main` (git on `PATH`) | before a hand-over in a repository, for its worktree (`src/worktree.cpp`) | the same, `GIT_TERMINAL_PROMPT=0` and `GCM_INTERACTIVE=never` (git never asks), ended after 60 s (fetch) and 120 s (add) |
| `git -C <worktree> status --porcelain`, `git -C <repo> worktree remove --force --force`, `worktree prune`, `rev-list --count <branch> --not --remotes`, `branch -D <branch>` | when such a run ends, at the end of ai-desktop, and at its next start for what was left | the same; the branch is deleted only when every commit of it is on a remote |
| `git -C <repo> show ... <sha>` (git on `PATH`) | a review (`review-done`), once per commit of the task | the same, ended after 20 s, output cut at 48 KiB; read-only |
| `nvim --headless ... scripts/testing.lua ci remote --json <repo>` (or the `testing` program named in `testing_cli`), `git -C <repo> merge-base --is-ancestor <sha> <head>` | a review (`review-done`), once per repository of its commits, for the state of the hosted CI (`src/ci_status.cpp`) | the same, ended after 60 s (`ci remote`, which asks GitHub with the person's token) and 20 s; read-only |
| the default browser with the dashboard | a new job and no dashboard window shown | `ShellExecuteW` of the `http://127.0.0.1:<port>/#job=<id>` address (`src/foreground.cpp`): the browser is the person's, not a child that ai-desktop ends |

There is no `system`, `popen` or `cmd.exe`/`sh` anywhere in `src/`: a `.cmd` for Claude Code or the tasks command is
refused instead of run through a shell. Any other process started by ai-desktop is a bug.

How it is started, and what stays visible on purpose:

| Who starts it | How | Window |
| --- | --- | --- |
| the Start menu link of the setup | `app\ai-desktop.exe` directly (the link sets the working folder) | its console: the server runs in it and ends when you close it |
| `ai-desktop.cmd` (the launcher of `scripts/setup.ps1` and of the setup) | `cmd.exe` runs the script, which starts `app\ai-desktop.exe` without putting `app\` (the runtime DLLs) on `PATH` | the console of the script; `cmd.exe` is the parent of ai-desktop, not its child |
| the tile of hub-desktop | `ai-desktop.exe` through `appkit_core::launch` with a console of its own (`Window::NewConsole`) | its console |

The console window is how the server is operated (a named exception of the plan "no visible
shell and background processes", rule `RS-08` (b) of the native ruleset): it is never started
hidden, and closing it ends the server. `conhost.exe` (or Windows Terminal) belongs to that
console.
