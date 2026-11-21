// A job of the model claude-code runs Claude Code headless with the brief of the tasks family (docs/JOBS.md, "Runs").
// The real binary against two stand-ins (tests/doubles/): fake_claude does what the agent should (reads the prompt,
// runs `tasks claim` and `tasks done` through the `tasks` on its PATH, writes stream-json), fake_tasks answers
// `agent-brief` and logs every other call. Nothing here starts the real Claude Code.
import assert from "node:assert/strict";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { after, before, describe, it } from "node:test";

import { NO_AUTH, binaryPath, startServer } from "./helpers/server.mjs";

const exe = process.platform === "win32" ? ".exe" : "";
const buildDir = path.dirname(binaryPath());
const fakeClaude = path.join(buildDir, `fake_claude${exe}`);
const fakeTasks = path.join(buildDir, `fake_tasks${exe}`);

const TASK = {
  source: "tasks-desktop",
  task_id: "demo/first-task",
  title: "First task",
  markdown: "---\ntitle: First task\n---\n\n## Akzeptanz\n\n- [ ] it is done\n",
  area: "demo",
  mode: "run",
};

// A throw-away world (a repository of the area: agent-worktree.test.mjs): a vault, the tasks stand-in as `tasks` in a bin folder, the logs, the config of the server.
function world({ claude = fakeClaude } = {}) {
  const root = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-agent-"));
  const vault = path.join(root, "tasks-vault");
  mkdirSync(vault);
  const bin = path.join(root, "bin");
  mkdirSync(bin);
  const tasks = path.join(bin, `tasks${exe}`);
  copyFileSync(fakeTasks, tasks);
  const configDir = path.join(root, "config");
  mkdirSync(configDir);
  writeFileSync(
    path.join(configDir, "config.json"),
    JSON.stringify({ version: 1, settings: { private_areas: "secret" } }),
  );
  return {
    claude,
    tasks,
    root,
    vault,
    bin,
    configDir,
    tasksLog: path.join(root, "tasks.log"),
    claudeLog: path.join(root, "claude.log"),
    pidFile: path.join(root, "claude.pid"),
    cleanup: () => rmSync(root, { recursive: true, force: true }),
  };
}

function lines(file) {
  return existsSync(file) ? readFileSync(file, "utf8").trim().split("\n").filter(Boolean).map((l) => JSON.parse(l)) : [];
}

async function serverFor(w, env = {}) {
  return startServer({
    configDir: w.configDir,
    env: {
      // The helper points claude_cli at nothing, so no test starts the real Claude Code; these name the stand-ins.
      AI_DESKTOP_CLAUDE_CLI: w.claude,
      AI_DESKTOP_TASKS_CLI: w.tasks,
      AI_DESKTOP_JOBS_PER_MINUTE: "1000",
      FAKE_TASKS_LOG: w.tasksLog,
      FAKE_CLAUDE_LOG: w.claudeLog,
      FAKE_CLAUDE_PID_FILE: w.pidFile,
      ...env,
    },
  });
}

function auth(server) {
  const secret = readFileSync(path.join(server.configDir, "control.secret"), "utf8").trim();
  return { Authorization: `Bearer ${secret}` };
}

async function waitFor(server, id, done, ms = 15000) {
  const deadline = Date.now() + ms;
  for (;;) {
    const job = (await server.get(`/jobs/${id}`)).json;
    if (done(job) || Date.now() > deadline) return job;
    await new Promise((r) => setTimeout(r, 100));
  }
}

function alive(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

describe("a job of the model claude-code", { skip: !existsSync(fakeClaude) && "build the test doubles first" }, () => {
  let w;
  let server;
  before(async () => {
    w = world();
    server = await serverFor(w);
  });
  after(async () => {
    await server?.stop();
    w?.cleanup();
  });

  it("runs Claude Code, which claims and finishes the task with the tasks command", async () => {
    const res = await server.post("/jobs", { ...TASK, vault: w.vault }, auth(server));
    assert.equal(res.status, 201, res.text);
    assert.equal(res.json.status, "running");
    const id = res.json.job_id;
    const job = await waitFor(server, id, (j) => j.status !== "running");
    assert.equal(job.status, "done", JSON.stringify(job));
    assert.equal(job.model, "claude-code", "jobs_model is claude-code unless set");
    assert.equal(job.runner, "claude-code");
    assert.deepEqual(job.brief, { brief_version: 2, contract_schema: 1, engine_version: "fake-0" },
      "no repository of the area: no --repo, no checks");
    assert.match(job.output, /Claude Code \(fake-model\)/);
    assert.match(job.output, /Bash: tasks claim demo\/first-task/);
    assert.match(job.output, /Did the work of demo\/first-task\./);

    const calls = lines(w.tasksLog).map((c) => c.args);
    assert.deepEqual(calls[0], ["claim", "demo/first-task", `--by=ai-desktop:${id}`]);
    assert.deepEqual(calls[1], ["done", "demo/first-task", "--done-in=fake", `--by=ai-desktop:${id}`]);
    for (const call of lines(w.tasksLog)) assert.equal(call.vault, w.vault, "TASKS_VAULT names the vault");

    const run = lines(w.claudeLog).at(-1);
    assert.equal(path.resolve(run.cwd), path.resolve(w.vault), "no repository of the area: the vault");
    assert.equal(path.resolve(run.path0), path.resolve(w.bin), "the tasks command comes first on PATH");
    assert.ok(run.args.includes("-p"));
    assert.ok(run.args.includes("stream-json"));
    assert.ok(!run.args.includes("--dangerously-skip-permissions"));
    assert.deepEqual(run.args.slice(run.args.indexOf("--allowedTools") + 1),
      ["Bash(tasks:*)", "Bash(git:*)", "Bash(scripts/test.sh ci commit:*)", "Bash(testing ci commit:*)",
        "Bash(scripts/test.sh ci land)", "Bash(testing ci land)", "Read", "Edit", "Write", "Glob", "Grep"]);
    const briefFile = run.args[run.args.indexOf("--append-system-prompt-file") + 1];
    assert.ok(!existsSync(briefFile), "the brief file is removed after the run");
    assert.match(run.prompt, /## Akzeptanz/);

    const usage = (await server.get("/usage?provider=claude-code")).json;
    assert.equal(usage.events[0].provider, "claude-code");
    assert.equal(usage.events[0].prompt_tokens, 12);
    assert.equal(usage.events[0].completion_tokens, 34);
  });

  it("asks before a private area goes to Claude Code", async () => {
    const res = await server.post("/jobs", { ...TASK, vault: w.vault, area: "secret" }, auth(server));
    assert.equal(res.json.status, "needs_confirmation");
    const before = lines(w.claudeLog).length;
    await new Promise((r) => setTimeout(r, 300));
    assert.equal(lines(w.claudeLog).length, before, "nothing started");
  });

  it("a model that is not claude-code answers once, as before", async () => {
    const res = await server.post("/jobs", { ...TASK, vault: w.vault, model: "llama3" }, auth(server));
    const job = await waitFor(server, res.json.job_id, (j) => j.status !== "running");
    assert.equal(job.runner, "model");
    assert.equal(job.status, "failed", "Ollama is not reachable in the tests");
  });
});

describe("Claude Code runs that end otherwise", { skip: !existsSync(fakeClaude) && "build the test doubles first" }, () => {
  it("a cancel ends the process tree of the agent", async () => {
    const w = world();
    const server = await serverFor(w, { FAKE_CLAUDE_SLEEP_MS: "30000" });
    try {
      const res = await server.post("/jobs", { ...TASK, vault: w.vault }, auth(server));
      const id = res.json.job_id;
      await waitFor(server, id, (j) => /tasks sleep/.test(j.output || ""));
      const pid = Number(readFileSync(w.pidFile, "utf8"));
      assert.ok(alive(pid));
      const blind = await server.post(`/jobs/${id}/cancel`, {}, NO_AUTH);
      assert.equal(blind.status, 401, "the dashboard or the secret");
      const forged = await server.post(`/jobs/${id}/cancel`, {}, { ...NO_AUTH, Origin: `http://127.0.0.1:${server.port}` });
      assert.equal(forged.status, 401, "the dashboard's Origin alone proves nothing");
      const cancelled = await server.post(`/jobs/${id}/cancel`, {}, await server.session());
      assert.equal(cancelled.status, 200, cancelled.text);
      assert.equal(cancelled.json.status, "cancelled");
      const deadline = Date.now() + 5000;
      while (alive(pid) && Date.now() < deadline) await new Promise((r) => setTimeout(r, 100));
      assert.ok(!alive(pid), "the agent is gone");
      await new Promise((r) => setTimeout(r, 300));
      const job = (await server.get(`/jobs/${id}`)).json;
      assert.equal(job.status, "cancelled", "the end of the run does not overwrite the cancel");
      assert.ok(!lines(w.tasksLog).some((c) => c.args[0] === "done"));
      assert.equal((await server.post(`/jobs/${id}/cancel`, {}, auth(server))).status, 409);

      // Run again: the new run gets a brief file of its own, so the end of the cancelled run (which removes its
      // file) can never take away the brief of the run started after it.
      const firstBrief = lines(w.claudeLog).at(-1).args;
      const again = await server.post(`/jobs/${id}/run`, {}, auth(server));
      assert.equal(again.status, 200, again.text);
      await waitFor(server, id, (j) => /tasks sleep/.test(j.output || ""));
      const secondBrief = lines(w.claudeLog).at(-1).args;
      const briefOf = (args) => args[args.indexOf("--append-system-prompt-file") + 1];
      assert.notEqual(briefOf(secondBrief), briefOf(firstBrief), "every run has its own brief file");
      assert.ok(existsSync(briefOf(secondBrief)), "the brief of the running run is there");
      assert.equal((await server.post(`/jobs/${id}/cancel`, {}, auth(server))).status, 200);
      const end = Date.now() + 5000;
      while (existsSync(briefOf(secondBrief)) && Date.now() < end) await new Promise((r) => setTimeout(r, 100));
      assert.ok(!existsSync(briefOf(firstBrief)) && !existsSync(briefOf(secondBrief)), "each run removed its own");
    } finally {
      await server.stop();
      w.cleanup();
    }
  });

  it("a descendant that keeps the output open does not hold the run once Claude Code ended", async () => {
    const w = world();
    // fake_claude leaves `tasks sleep 20000` behind with its stdout and stderr, then ends with its result.
    const server = await serverFor(w, { FAKE_CLAUDE_LEAVE_MS: "20000" });
    try {
      const started = Date.now();
      const res = await server.post("/jobs", { ...TASK, vault: w.vault }, auth(server));
      assert.equal(res.status, 201, res.text);
      const job = await waitFor(server, res.json.job_id, (j) => j.status !== "running", 8000);
      assert.equal(job.status, "done", JSON.stringify(job));
      assert.ok(Date.now() - started < 8000, "the run ended with Claude Code, not with its descendant");
      assert.match(job.output, /Did the work of demo\/first-task/, "the output written before the end is kept");
    } finally {
      await server.stop();
      w.cleanup();
    }
  });

  it("an error result fails the job", async () => {
    const w = world();
    const server = await serverFor(w, { FAKE_CLAUDE_FAIL: "1" });
    try {
      const res = await server.post("/jobs", { ...TASK, vault: w.vault }, auth(server));
      const job = await waitFor(server, res.json.job_id, (j) => j.status !== "running");
      assert.equal(job.status, "failed");
      assert.equal(job.error, "Claude Code: boom");
    } finally {
      await server.stop();
      w.cleanup();
    }
  });

  for (const [name, env, worldOptions, pattern] of [
    ["an engine without agent-brief", { FAKE_TASKS_NO_BRIEF: "1" }, {}, /agent-brief failed/],
    ["an older engine", { FAKE_TASKS_CONTRACT: "0" }, {}, /older than ai-desktop expects/],
    ["a brief without checks (brief 1)", { FAKE_TASKS_BRIEF_VERSION: "1" }, {}, /older than ai-desktop expects/],
    ["no Claude Code", {}, { claude: path.join(os.tmpdir(), `no-such-claude${exe}`) }, /claude_cli names no file/],
  ]) {
    it(`with ${name} the job answers once and says why`, async () => {
      const w = world(worldOptions);
      const server = await serverFor(w, env);
      try {
        const res = await server.post("/jobs", { ...TASK, vault: w.vault }, auth(server));
        const job = await waitFor(server, res.json.job_id, (j) => j.status !== "running");
        assert.equal(job.runner, "model");
        assert.match(job.note, pattern);
        assert.match(job.note, /one answer of/);
        assert.equal(lines(w.claudeLog).length, 0, "Claude Code did not run");
      } finally {
        await server.stop();
        w.cleanup();
      }
    });
  }

  it("a vault that does not exist here answers once and says so", async () => {
    const w = world();
    const server = await serverFor(w);
    try {
      const res = await server.post("/jobs", { ...TASK, vault: path.join(w.root, "elsewhere") }, auth(server));
      const job = await waitFor(server, res.json.job_id, (j) => j.status !== "running");
      assert.match(job.note, /does not exist here/);
    } finally {
      await server.stop();
      w.cleanup();
    }
  });
});
