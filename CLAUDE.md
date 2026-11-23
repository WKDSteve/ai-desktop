# ai-desktop

Local agent-framework server in C++17 (`cpp-httplib`, CMake) with a live dashboard (SSE) in vanilla JS. The README
(`README.md`, German: `README.de.md`) is the reference for setup, API and settings; `docs/JOBS.md` and
`docs/PROCESSES.md` describe jobs and every process the server starts.

## Layout
- `src/` the server; `src/main.cpp` sets the dashboard's Content-Security-Policy (scripts and styles of this
  origin only, no inline script/handler/style, Trusted Types `'none'`). Dashboard code must stay within it.
- `dashboard/` the UI: `index.html`, `app.js`, `i18n.js`, `style.css`, `palette.css` (colour tokens). No build step,
  no package manager.
- `tests/` unit tests (ctest) and API tests (`node --test tests/*.test.mjs`, they start the real binary);
  `e2e/` Playwright + axe checks of the dashboard (its own `package.json`, test tooling only).
- `tools/` hygiene checks, `scripts/` setup/build, `installer/` the Windows installer.

## Checks
What CI runs (`.github/workflows/ci.yml`, `pr.yml`): build via `scripts/setup.ps1` / `scripts/setup.sh`, `ctest`, the
API tests, and the hygiene checks:
`python -m unittest discover -s tools -p "test_*.py"`, `python tools/check-control-chars.py`,
`python tools/check-color-literals.py --root dashboard --tokens dashboard/palette.css`,
`python tools/check-dashboard-size.py` (size budget of `dashboard/`, raised on purpose in the commit that needs more),
`python tools/check-release-kit-pin.py`, `python tools/check-install-runners.py`.
Colours in the dashboard come from `palette.css` tokens only.

## Commits
Conventional subjects (`feat(scope): ...`, `test(...)`, `fix(...)`), a body that says what changed and why.

## Web platform
- Browser Support: Baseline Widely available (`.mwgrc`; no `browserslist`, the dashboard has no package.json or
  build). Newer features only behind feature detection with a fallback; polyfills where browsers still in use need them (rather one too many), run only after the check: dynamic `import()`, or bundled in a `file://` page.
- Before writing or changing HTML, CSS or client-side JS, use the `modern-web-guidance` skill (`search` → `retrieve` →
  verify). Not for C++, backend, CI or build config.
- For open-ended modernisation: `npx -y modern-web-guidance@latest legacy-scan dashboard --json`, check each finding
  against `keepWhen`/`dropWhen`. The scan runs only locally / in agent sessions; there is no CI scanner here (no
  package manager in this repo).
- Known findings: none open. The log's scrollbar uses `scrollbar-width`/`scrollbar-color` (no `::-webkit-scrollbar`), and
  the prompt is a `<textarea>` with no Enter handler (Enter inserts a newline, there is no submit-on-Enter to make
  IME-safe). A submit on Enter added later must ignore `event.isComposing`.
- Rules: the maintainer's ruleset for the web platform (private, so not named or cited here by id).
- `.claude/settings.json` turns the tool's telemetry off and allows only `npx -y modern-web-guidance@latest *`.
- Git: in a cloud session (`CLAUDE_CODE_REMOTE` is set) never push to `main`, only to your own `claude/…` branch; a local session lands it by rebase and the device key re-signs it (`GIT-09`). "Push to main" (`GIT-01`) applies locally only.
