// The command in the terminal: `ai-desktop start|stop|status|open|--version|--help`, and the starters the setup
// writes with `cli-install` (src/cli.hpp). Real binary, a throw-away config folder and starter folder: the tests never
// touch the PATH, the Start menu or the real family folder of the machine.
import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { describe, it } from "node:test";

import { binaryPath, freePort, repoRoot, request } from "./helpers/server.mjs";

const isWindows = process.platform === "win32";

function hermeticEnv(extra = {}) {
  return {
    ...process.env,
    AI_DESKTOP_DASHBOARD_DIR: path.join(repoRoot, "dashboard"),
    AI_DESKTOP_OLLAMA_HOST: "http://127.0.0.1:9",
    AI_DESKTOP_SECRET_BACKEND: "file",
    AI_DESKTOP_NO_BROWSER: "1",
    AI_DESKTOP_JOBS_FOREGROUND: "0",
    AI_DESKTOP_JOBS: "",
    OPENAI_API_KEY: "",
    ANTHROPIC_API_KEY: "",
    GEMINI_API_KEY: "",
    ...extra,
  };
}

// Runs the command to its end (these commands all return).
function ai(args, env) {
  const result = spawnSync(binaryPath(), args, { env: hermeticEnv(env), cwd: os.tmpdir(), encoding: "utf8", timeout: 60000 });
  return { code: result.status, out: result.stdout ?? "", err: result.stderr ?? "" };
}

async function withBackground(fn) {
  const configDir = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-cli-"));
  const port = await freePort();
  const env = { AI_DESKTOP_CONFIG_DIR: configDir, AI_DESKTOP_PORT: String(port) };
  try {
    await fn({ configDir, port, env });
  } finally {
    ai(["stop"], env);
    // A server that did not stop: end it by the pid of its record.
    try {
      const record = JSON.parse(readFileSync(path.join(configDir, "hub.json"), "utf8"));
      process.kill(record.pid);
    } catch {
      // gone
    }
    await new Promise((r) => setTimeout(r, 300));
    rmSync(configDir, { recursive: true, force: true });
  }
}

describe("ai-desktop in the terminal", () => {
  it("--version and --help answer and exit 0, wrong use exits 2", () => {
    const version = ai(["--version"]);
    assert.equal(version.code, 0);
    assert.match(version.out, /^ai-desktop \d+\.\d+\.\d+/);
    assert.equal(ai(["-V"]).out, version.out);
    const help = ai(["--help"]);
    assert.equal(help.code, 0);
    for (const word of ["start", "stop", "status", "open", "--port", "--version"]) assert.ok(help.out.includes(word), word);
    for (const args of [["frobnicate"], ["stop", "--now"], ["status", "--open"], ["start", "--port"]]) {
      const bad = ai(args);
      assert.equal(bad.code, 2, args.join(" "));
      assert.match(bad.err, /ai-desktop: /);
    }
  });

  it("status says not running with exit 1, in text and in JSON", async () => {
    const configDir = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-cli-"));
    try {
      const env = { AI_DESKTOP_CONFIG_DIR: configDir };
      const text = ai(["status"], env);
      assert.equal(text.code, 1);
      assert.match(text.out, /not running/);
      const json = ai(["status", "--json"], env);
      assert.equal(json.code, 1);
      assert.equal(JSON.parse(json.out).running, false);
      // A stop of nothing is fine.
      assert.equal(ai(["stop"], env).code, 0);
    } finally {
      rmSync(configDir, { recursive: true, force: true });
    }
  });

  it("start runs it in a console window of its own, status sees it, stop ends it, a second start is no second server", async () => {
    await withBackground(async ({ configDir, port, env }) => {
      const started = ai(["start"], env);
      assert.equal(started.code, 0, started.err);
      assert.match(started.out, /started/);

      const status = ai(["status", "--json"], env);
      assert.equal(status.code, 0);
      const doc = JSON.parse(status.out);
      assert.equal(doc.running, true);
      assert.equal(doc.port, port);
      assert.equal(doc.url, `http://127.0.0.1:${port}/`);
      assert.equal(doc.pid, JSON.parse(readFileSync(path.join(configDir, "hub.json"), "utf8")).pid);
      assert.equal((await request(port, { pathname: "/health" })).status, 200);

      const again = ai(["start"], env);
      assert.equal(again.code, 0);
      assert.match(again.out, /already running/);
      assert.equal(JSON.parse(ai(["status", "--json"], env).out).pid, doc.pid);

      const stopped = ai(["stop"], env);
      assert.equal(stopped.code, 0, stopped.err);
      assert.match(stopped.out, /stopped/);
      assert.equal(ai(["status"], env).code, 1);
      assert.ok(!existsSync(path.join(configDir, "hub.json")), "hub.json goes with the server");
      await assert.rejects(request(port, { pathname: "/health" }));
    });
  });

  it("quit is stop, and start --open / open name the dashboard (and start it when it is not running)", async () => {
    await withBackground(async ({ port, env }) => {
      const opened = ai(["open"], env);
      assert.equal(opened.code, 0, opened.err);
      assert.ok(opened.out.includes(`Dashboard: http://127.0.0.1:${port}/`), opened.out);
      assert.ok(!opened.out.includes("ticket"), "no ticket is printed");
      assert.equal(ai(["status"], env).code, 0);
      const withOpen = ai(["start", "--open"], env);
      assert.equal(withOpen.code, 0);
      assert.ok(withOpen.out.includes("Dashboard:"));
      assert.equal(ai(["quit"], env).code, 0);
      assert.equal(ai(["status"], env).code, 1);
    });
  });

  it("start on a taken port exits 1", async () => {
    await withBackground(async ({ env }) => {
      assert.equal(ai(["start"], env).code, 0);
      const other = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-cli-other-"));
      try {
        const second = ai(["start"], { ...env, AI_DESKTOP_CONFIG_DIR: other });
        assert.equal(second.code, 1);
        assert.match(second.err, /ended right after it started|did not answer/);
      } finally {
        rmSync(other, { recursive: true, force: true });
      }
    });
  });

  it("POST /shutdown needs the control secret", async () => {
    await withBackground(async ({ configDir, port, env }) => {
      assert.equal(ai(["start"], env).code, 0);
      const refused = await request(port, { method: "POST", pathname: "/shutdown" });
      assert.equal(refused.status, 401);
      const wrong = await request(port, { method: "POST", pathname: "/shutdown", headers: { Authorization: "Bearer " + "0".repeat(64) } });
      assert.equal(wrong.status, 401);
      assert.equal(ai(["status"], env).code, 0, "still running");
      assert.ok(existsSync(path.join(configDir, "hub.json")));
    });
  });
});

describe("the starters of cli-install", () => {
  it("are written into APPKIT_FAMILY_DIR/bin, run the program, hand its exit code back and go with cli-uninstall", () => {
    const family = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-family-"));
    try {
      const env = { APPKIT_FAMILY_DIR: family };
      const installed = ai(["cli-install"], env);
      assert.equal(installed.code, 0, installed.err);
      const bin = path.join(family, "bin");
      const sh = path.join(bin, "ai-desktop");
      assert.ok(existsSync(sh));
      assert.match(readFileSync(sh, "utf8"), /appkit starter/);
      if (isWindows) {
        const cmd = path.join(bin, "ai-desktop.cmd");
        const text = readFileSync(cmd, "latin1");
        assert.match(text, /appkit starter/);
        assert.ok(text.includes("\r\n") && !/[^\r]\n/.test(text), "CRLF only");
        assert.ok(!text.includes("\\\\?\\"), "no verbatim path");
        // The real thing: cmd runs it, the program answers, exit codes come back.
        const run = spawnSync("cmd.exe", ["/d", "/c", cmd, "--version"], { env: hermeticEnv(), encoding: "utf8" });
        assert.equal(run.status, 0, run.stderr);
        assert.match(run.stdout, /^ai-desktop \d/);
        const bad = spawnSync("cmd.exe", ["/d", "/c", cmd, "frobnicate"], { env: hermeticEnv(), encoding: "utf8" });
        assert.equal(bad.status, 2);
      } else {
        const run = spawnSync("sh", [sh, "--version"], { env: hermeticEnv(), encoding: "utf8" });
        assert.equal(run.status, 0, run.stderr);
        assert.match(run.stdout, /^ai-desktop \d/);
        assert.equal(spawnSync("sh", [sh, "frobnicate"], { env: hermeticEnv() }).status, 2);
      }

      // Installing again is fine; a program that is gone ends a starter with 3.
      assert.equal(ai(["cli-install", "--program", path.join(family, "gone", "ai-desktop")], env).code, 0);
      const gone = isWindows
        ? spawnSync("cmd.exe", ["/d", "/c", path.join(bin, "ai-desktop.cmd"), "--version"], { env: hermeticEnv(), encoding: "utf8" })
        : spawnSync("sh", [sh, "--version"], { env: hermeticEnv(), encoding: "utf8" });
      assert.equal(gone.status, 3);
      assert.match(gone.stderr, /its program is gone/);

      // Uninstall takes the starters and the empty folder; a foreign file keeps the folder.
      writeFileSync(path.join(bin, "other-tool"), "not ours\n");
      assert.equal(ai(["cli-uninstall"], env).code, 0);
      assert.ok(!existsSync(sh));
      assert.ok(existsSync(path.join(bin, "other-tool")));
      rmSync(path.join(bin, "other-tool"));
      assert.equal(ai(["cli-install"], env).code, 0);
      assert.equal(ai(["cli-uninstall"], env).code, 0);
      assert.ok(!existsSync(bin), "the empty folder goes too");
    } finally {
      rmSync(family, { recursive: true, force: true });
    }
  });
});
