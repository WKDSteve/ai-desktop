// Test harness: starts the real ai-desktop binary on a free port, with a throw-away
// config directory, no API keys and an unreachable Ollama, so tests never
// depend on (or touch) the developer's own setup.
import { spawn } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import http from "node:http";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
export const repoRoot = path.resolve(here, "..", "..");

export function binaryPath() {
  if (process.env.AI_DESKTOP_BIN) return process.env.AI_DESKTOP_BIN;
  const exe = process.platform === "win32" ? "ai-desktop.exe" : "ai-desktop";
  return path.join(repoRoot, "build", exe);
}

export function freePort() {
  return new Promise((resolve, reject) => {
    const probe = net.createServer();
    probe.once("error", reject);
    probe.listen(0, "127.0.0.1", () => {
      const { port } = probe.address();
      probe.close(() => resolve(port));
    });
  });
}

// Minimal HTTP client. Not fetch(): undici refuses a hand-set Origin header in
// some versions, and these tests need exactly that.
export function request(port, { method = "GET", pathname = "/", headers = {}, body } = {}) {
  return new Promise((resolve, reject) => {
    const payload = body === undefined ? undefined : typeof body === "string" ? body : JSON.stringify(body);
    const req = http.request(
      {
        host: "127.0.0.1",
        port,
        method,
        path: pathname,
        // A header given as undefined is left out (NO_AUTH).
        headers: Object.fromEntries(
          Object.entries({
            ...(payload !== undefined ? { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(payload) } : {}),
            ...headers,
          }).filter(([, value]) => value !== undefined),
        ),
      },
      (res) => {
        const chunks = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => {
          const text = Buffer.concat(chunks).toString("utf8");
          let json;
          try {
            json = JSON.parse(text);
          } catch {
            json = undefined;
          }
          resolve({ status: res.statusCode, headers: res.headers, text, json });
        });
      },
    );
    req.on("error", reject);
    if (payload !== undefined) req.write(payload);
    req.end();
  });
}

// The headers of a request without any proof: the server's get/post/put/del send the control secret unless the
// headers name an Authorization of their own (this one leaves it out).
export const NO_AUTH = Object.freeze({ Authorization: undefined });

// Starts the server; resolves once /health answers. `env` overrides the
// hermetic defaults, `args` are passed on the command line.
export async function startServer({ env = {}, args = [], port, configDir, bin = binaryPath() } = {}) {
  if (!existsSync(bin)) {
    throw new Error(`ai-desktop binary not found at ${bin} (build it first, or set AI_DESKTOP_BIN)`);
  }
  const usedPort = port ?? (await freePort());
  const ownsConfigDir = !configDir;
  const usedConfigDir = configDir ?? mkdtempSync(path.join(os.tmpdir(), "ai-desktop-test-"));

  const child = spawn(bin, args, {
    // Run from somewhere that is NOT the repo, so the tests prove that
    // AI_DESKTOP_DASHBOARD_DIR works and nothing relies on ./dashboard.
    cwd: os.tmpdir(),
    env: {
      ...process.env,
      AI_DESKTOP_PORT: String(usedPort),
      AI_DESKTOP_CONFIG_DIR: usedConfigDir,
      AI_DESKTOP_DASHBOARD_DIR: path.join(repoRoot, "dashboard"),
      AI_DESKTOP_OLLAMA_HOST: "http://127.0.0.1:9", // nothing listens on the discard port
      // Keys saved by a test go into <config dir>/secrets.json, never into the
      // developer's real Windows Credential Manager.
      AI_DESKTOP_SECRET_BACKEND: "file",
      OPENAI_API_KEY: "",
      ANTHROPIC_API_KEY: "",
      GEMINI_API_KEY: "",
      // A job brings the dashboard to the front (opens the browser); never in a test.
      AI_DESKTOP_JOBS_FOREGROUND: "0",
      AI_DESKTOP_JOBS: "",
      AI_DESKTOP_PRIVATE_AREAS: "",
      AI_DESKTOP_JOBS_MODEL: "",
      // The default model of the machine the tests run on (a developer's or a runner's user environment) must not
      // reach the server: a test that wants one names it in `env`.
      AI_DESKTOP_DEFAULT_MODEL: "",
      // A job of the model claude-code must never start the real Claude Code of the machine: a path that does not
      // exist (the job then answers once); tests/agent-jobs.test.mjs names its stand-in instead.
      AI_DESKTOP_CLAUDE_CLI: path.join(os.tmpdir(), "ai-desktop-tests-no-claude", "claude.exe"),
      AI_DESKTOP_TASKS_CLI: "",
      // Nor the testing command of the machine (a review asks it for the hosted CI, which reaches GitHub): a path that
      // does not exist (the review then says "not checked"); tests/review-jobs.test.mjs names its stand-in instead.
      AI_DESKTOP_TESTING_CLI: path.join(os.tmpdir(), "ai-desktop-tests-no-testing", "testing.exe"),
      ...env,
    },
    stdio: ["ignore", "pipe", "pipe"],
  });
  let output = "";
  child.stdout.on("data", (c) => (output += c));
  child.stderr.on("data", (c) => (output += c));

  const exited = new Promise((resolve) => child.once("exit", (code) => resolve(code)));

  const deadline = Date.now() + 15000;
  let ready = false;
  while (Date.now() < deadline && child.exitCode === null) {
    try {
      const res = await request(usedPort, { pathname: "/health" });
      if (res.status === 200) {
        ready = true;
        break;
      }
    } catch {
      // not listening yet
    }
    await new Promise((r) => setTimeout(r, 100));
  }
  if (!ready) {
    child.kill();
    throw new Error(`server did not become ready; output:\n${output}`);
  }

  // The management API wants a proof (src/session.hpp): the tests send the control secret, as a family app does,
  // unless they name an Authorization themselves.
  const secret = () => {
    try {
      return readFileSync(path.join(usedConfigDir, "control.secret"), "utf8").trim();
    } catch {
      return "";
    }
  };
  const withAuth = (headers = {}) =>
    "Authorization" in headers || !secret() ? headers : { Authorization: `Bearer ${secret()}`, ...headers };

  return {
    port: usedPort,
    configDir: usedConfigDir,
    output: () => output,
    get: (pathname, headers) => request(usedPort, { pathname, headers: withAuth(headers) }),
    post: (pathname, body, headers) => request(usedPort, { method: "POST", pathname, body, headers: withAuth(headers) }),
    put: (pathname, body, headers) => request(usedPort, { method: "PUT", pathname, body, headers: withAuth(headers) }),
    del: (pathname, headers) => request(usedPort, { method: "DELETE", pathname, headers: withAuth(headers) }),
    // The headers of the dashboard after its login: a session token for a ticket, as the page gets it.
    async session() {
      const url = await this.dashboardUrl();
      const ticket = new URL(url).hash.match(/ticket=([0-9a-f]+)/)[1];
      const res = await request(usedPort, { method: "POST", pathname: "/session", body: { ticket } });
      if (res.status !== 200) throw new Error(`no session: ${res.status} ${res.text}`);
      return { Authorization: `Bearer ${res.json.token}` };
    },
    // The address of the dashboard with a one-time ticket (as the hub's tile opens it); `hash` without "#", e.g.
    // "job=<id>".
    async dashboardUrl(hash = "") {
      const res = await request(usedPort, {
        method: "POST",
        pathname: "/session/ticket",
        headers: { Authorization: `Bearer ${secret()}` },
      });
      if (res.status !== 200) throw new Error(`no ticket: ${res.status} ${res.text}`);
      const fragment = (hash ? hash + "&" : "") + "ticket=" + res.json.ticket;
      return `http://127.0.0.1:${usedPort}/#${fragment}`;
    },
    async stop() {
      if (child.exitCode === null) child.kill();
      await exited;
      if (ownsConfigDir) rmSync(usedConfigDir, { recursive: true, force: true });
    },
  };
}

// Runs the binary expecting it to exit by itself (bad arguments, ...).
export function runToExit(args, env = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(binaryPath(), args, {
      cwd: os.tmpdir(),
      env: { ...process.env, AI_DESKTOP_DASHBOARD_DIR: path.join(repoRoot, "dashboard"), ...env },
      stdio: ["ignore", "pipe", "pipe"],
    });
    let output = "";
    child.stdout.on("data", (c) => (output += c));
    child.stderr.on("data", (c) => (output += c));
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error(`binary did not exit; output:\n${output}`));
    }, 10000);
    child.once("exit", (code) => {
      clearTimeout(timer);
      resolve({ code, output });
    });
  });
}
