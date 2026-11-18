// A job of the kind review-done (docs/JOBS.md "Reviews"): the real binary reads the diffs of the task's commits from a
// throw-away git repository, and the agent (fake_claude) only reads and answers with the review block; a model that is
// not claude-code gets the same prompt for one answer. Nothing starts the real Claude Code.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { after, before, describe, it } from "node:test";

import { binaryPath, startServer } from "./helpers/server.mjs";

const exe = process.platform === "win32" ? ".exe" : "";
const fakeClaude = path.join(path.dirname(binaryPath()), `fake_claude${exe}`);
const fakeTasks = path.join(path.dirname(binaryPath()), `fake_tasks${exe}`);

function git(cwd, args) {
  return execFileSync("git", ["-c", "user.name=t", "-c", "user.email=t@example.org", "-c", "commit.gpgsign=false", ...args], {
    cwd,
    encoding: "utf8",
  }).trim();
}

function lines(file) {
  return existsSync(file) ? readFileSync(file, "utf8").trim().split("\n").filter(Boolean).map((l) => JSON.parse(l)) : [];
}

// An OpenAI-compatible local model that records the prompt it got and answers with a review block.
function startMock() {
  const seen = [];
  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => {
      seen.push(JSON.parse(Buffer.concat(chunks).toString("utf8") || "{}"));
      res.writeHead(200, { "Content-Type": "text/event-stream" });
      res.write(`data: ${JSON.stringify({ choices: [{ delta: { content: "<!-- review:begin -->\n## Prüfung\n<!-- review:end -->" } }] })}\n\n`);
      res.end("data: [DONE]\n\n");
    });
  });
  return new Promise((resolve) =>
    server.listen(0, "127.0.0.1", () => resolve({ port: server.address().port, seen, close: () => new Promise((r) => server.close(r)) })),
  );
}

describe("a job of the kind review-done", { skip: !existsSync(fakeClaude) && "build the test doubles first" }, () => {
  let root;
  let vault;
  let repo;
  let sha;
  let server;
  let mock;
  const claudeLog = () => path.join(root, "claude.log");

  before(async () => {
    root = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-review-"));
    vault = path.join(root, "tasks-vault");
    repo = path.join(root, "demo");
    mkdirSync(vault);
    mkdirSync(repo);
    git(repo, ["init", "-q"]);
    writeFileSync(path.join(repo, "feature.txt"), "the acceptance is met here\n");
    git(repo, ["add", "feature.txt"]);
    git(repo, ["commit", "-q", "-m", "feat: the feature"]);
    sha = git(repo, ["rev-parse", "--short=12", "HEAD"]);
    mock = await startMock();
    // The tasks command (a stand-in): split-plan puts its brief, the allowed values, into the prompt.
    mkdirSync(path.join(root, "bin"));
    copyFileSync(fakeTasks, path.join(root, "bin", `tasks${exe}`));
    const configDir = path.join(root, "config");
    mkdirSync(configDir);
    writeFileSync(
      path.join(configDir, "config.json"),
      JSON.stringify({
        version: 1,
        settings: {},
        custom_models: [{ id: "mock-local", provider: "openai-compatible", base_url: `http://127.0.0.1:${mock.port}/v1` }],
      }),
    );
    server = await startServer({
      configDir,
      env: {
        AI_DESKTOP_CLAUDE_CLI: fakeClaude,
        AI_DESKTOP_TASKS_CLI: path.join(root, "bin", `tasks${exe}`),
        AI_DESKTOP_JOBS_PER_MINUTE: "1000",
        FAKE_CLAUDE_LOG: claudeLog(),
        FAKE_TASKS_LOG: path.join(root, "tasks.log"),
      },
    });
  });
  after(async () => {
    await server?.stop();
    await mock?.close();
    rmSync(root, { recursive: true, force: true });
  });

  function task(model) {
    return {
      source: "tasks.nvim",
      task_id: "demo/finished",
      title: "Finished",
      markdown: `---\ntitle: Finished\nstatus: done\ncommits: [demo@${sha}, absent@1234567]\n---\n\n## Akzeptanz\n\n- [x] it is done\n- [x] it is documented\n`,
      vault,
      area: "demo",
      mode: "run",
      kind: "review-done",
      ...(model ? { model } : {}),
    };
  }

  async function run(body) {
    const secret = readFileSync(path.join(server.configDir, "control.secret"), "utf8").trim();
    const res = await server.post("/jobs", body, { Authorization: `Bearer ${secret}` });
    assert.equal(res.status, 201, res.text);
    const deadline = Date.now() + 15000;
    for (;;) {
      const job = (await server.get(`/jobs/${res.json.job_id}`)).json;
      if (job.status !== "running" || Date.now() > deadline) return job;
      await new Promise((r) => setTimeout(r, 100));
    }
  }

  it("Claude Code reads the diffs and answers with the review block, and writes nothing", async () => {
    const job = await run(task());
    assert.equal(job.status, "done", JSON.stringify(job));
    assert.equal(job.runner, "claude-code");
    assert.match(job.note, /read only/);
    assert.equal(job.brief, undefined, "a review needs no brief");
    assert.match(job.output, /<!-- review:begin -->\n## Prüfung/);
    assert.match(job.output, /<!-- review:end -->/);
    assert.match(job.output, /▸ Read: feature\.txt/);

    const call = lines(claudeLog()).at(-1);
    assert.equal(path.resolve(call.cwd), path.resolve(repo), "in the repository of the area");
    const allowed = call.args.slice(call.args.indexOf("--allowedTools") + 1);
    assert.deepEqual(allowed, ["Read", "Glob", "Grep"]);
    const disallowed = call.args.slice(call.args.indexOf("--disallowedTools") + 1, call.args.indexOf("--allowedTools"));
    for (const tool of ["Bash", "Edit", "Write", "WebFetch"]) assert.ok(disallowed.includes(tool), tool);
    const dirs = call.args.flatMap((a, i) => (a === "--add-dir" ? [path.resolve(call.args[i + 1])] : []));
    assert.deepEqual(dirs.sort(), [path.resolve(repo), path.resolve(vault)].sort());
    assert.match(call.prompt, /\+the acceptance is met here/, "the diff of the commit");
    assert.match(call.prompt, /No repository absent/, "a commit of a repository that is not here");
    assert.match(call.prompt, /change no file/);
    assert.equal(lines(path.join(root, "tasks.log")).length, 0, "no tasks command: nothing claimed or finished");
  });

  it("prepare-decision reads the task's references and answers with the decision block, read only", async () => {
    const job = await run({
      ...task(),
      task_id: "demo/choose",
      title: "Choose",
      kind: "prepare-decision",
      markdown: `---\ntitle: Choose\nstatus: decision\nrefs: [docs/x.md, demo@${sha}]\n---\n\nWhich store?\n`,
    });
    assert.equal(job.status, "done", JSON.stringify(job));
    assert.equal(job.runner, "claude-code");
    assert.match(job.note, /decision: read only/);
    assert.match(job.output, /<!-- decision:begin -->\n## Vorschlag/);
    assert.match(job.output, /## Offene Fragen\n\n### Which store\? \[q1\]/);
    const call = lines(claudeLog()).at(-1);
    assert.deepEqual(call.args.slice(call.args.indexOf("--allowedTools") + 1), ["Read", "Glob", "Grep"]);
    assert.ok(call.args.includes("Edit") && call.args.indexOf("Edit") < call.args.indexOf("--allowedTools"), "Edit refused");
    assert.match(call.prompt, /decide nothing/);
    assert.match(call.prompt, /\+the acceptance is met here/, "the diff of the commit its refs name");
    assert.match(call.prompt, /Which store\?/);
    const dirs = call.args.flatMap((a, i) => (a === "--add-dir" ? [path.resolve(call.args[i + 1])] : []));
    assert.ok(dirs.includes(path.resolve(repo)), "the repository of the area");
  });

  it("split-plan reads the plan with its tasks and the allowed values, and answers with JSON drafts", async () => {
    const plan = [
      "---",
      "title: The plan",
      "status: doing",
      "phase_order: [build]",
      "---",
      "",
      "Goal: the thing works.",
      "",
      "## Tasks of this plan",
      "",
      "- demo/existing (open, phase build): Existing work",
    ].join("\n");
    const job = await run({ ...task(), task_id: "demo/the-plan", title: "The plan", kind: "split-plan", markdown: plan });
    assert.equal(job.status, "done", JSON.stringify(job));
    assert.match(job.note, /split: read only/);
    assert.match(job.output, /<!-- split:begin -->/);
    const call = lines(claudeLog()).at(-1);
    assert.deepEqual(call.args.slice(call.args.indexOf("--allowedTools") + 1), ["Read", "Glob", "Grep"]);
    assert.match(call.prompt, /create no task/);
    assert.match(call.prompt, /demo\/existing \(open, phase build\)/, "the plan's tasks, against doubles");
    assert.match(call.prompt, /Tasks family \(fake\)/, "the brief with the allowed values");
    assert.equal(lines(path.join(root, "tasks.log")).length, 0, "the brief only: no claim, no new task");
  });

  it("another model gets the same prompt for one answer", async () => {
    const job = await run(task("mock-local"));
    assert.equal(job.status, "done", JSON.stringify(job));
    assert.equal(job.runner, "model");
    assert.match(job.output, /review:begin/);
    const asked = mock.seen.at(-1).messages.map((m) => m.content).join("\n");
    assert.match(asked, /\+the acceptance is met here/);
    assert.match(asked, /careful reviewer/, "the review's system text");
  });
});
