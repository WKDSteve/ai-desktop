# Programs ai-desktop starts

**None.** ai-desktop does its work in its own process: the HTTP server, the dashboard it serves,
and the calls to the model providers (Anthropic, OpenAI, Gemini, Ollama) over HTTP. The code
(`src/`) has no `CreateProcess`, `ShellExecute`, `system` or `popen`. **A process started by
ai-desktop is a bug.** Checked against a measurement with `app-proc-watch`.

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
