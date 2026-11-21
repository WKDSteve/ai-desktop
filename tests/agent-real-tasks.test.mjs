// A job of claude-code against the REAL tasks command of tasks.nvim (with fake_claude in place of Claude Code): the
// brief comes from `tasks agent-brief`, and the agent's `tasks claim` and `tasks done` change a throw-away vault for
// real. Runs only when AI_DESKTOP_TEST_TASKS_CLI names the bin/tasks (or bin/tasks.cmd) of a tasks.nvim checkout that
// has agent-brief, and nvim is on PATH; the other tests use the fake_tasks stand-in.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { describe, it } from "node:test";

import { binaryPath, startServer } from "./helpers/server.mjs";

const tasksCli = process.env.AI_DESKTOP_TEST_TASKS_CLI;
const exe = process.platform === "win32" ? ".exe" : "";
const fakeClaude = path.join(path.dirname(binaryPath()), `fake_claude${exe}`);
const skip = !tasksCli
  ? "AI_DESKTOP_TEST_TASKS_CLI is not set"
  : !existsSync(fakeClaude)
    ? "build the test doubles first"
    : false;

function tasks(vault, args) {
  const script = path.join(path.dirname(tasksCli), "..", "scripts", "tasks.lua");
  return execFileSync("nvim", ["-n", "-i", "NONE", "--headless", "-u", "NONE", "-l", script, ...args], {
    env: { ...process.env, TASKS_VAULT: vault },
    encoding: "utf8",
  });
}

describe("a job of claude-code with the real tasks command", { skip }, () => {
  it("the agent claims the task and finishes it in the vault", async () => {
    const root = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-real-tasks-"));
    const vault = path.join(root, "vault");
    tasks(vault, ["init-vault", vault, "--area=demo", "--lang=en", "--apply"]);
    const created = tasks(vault, ["new", "demo", "Handed over by a test", "--kind=task", "--prio=2", "--effort=XS"]);
    const id = created.split("\t")[1];
    assert.match(id, /^demo\//, created);
    const server = await startServer({
      env: { AI_DESKTOP_CLAUDE_CLI: fakeClaude, AI_DESKTOP_TASKS_CLI: tasksCli, AI_DESKTOP_JOBS_FOREGROUND: "0" },
    });
    try {
      const secret = readFileSync(path.join(server.configDir, "control.secret"), "utf8").trim();
      const res = await server.post(
        "/jobs",
        { source: "tasks-desktop", task_id: id, title: "Handed over", markdown: "## Acceptance\n", vault, area: "demo", mode: "run" },
        { Authorization: `Bearer ${secret}` },
      );
      assert.equal(res.status, 201, res.text);
      let job;
      const deadline = Date.now() + 60000;
      do {
        await new Promise((r) => setTimeout(r, 250));
        job = (await server.get(`/jobs/${res.json.job_id}`)).json;
      } while (job.status === "running" && Date.now() < deadline);
      assert.equal(job.status, "done", JSON.stringify(job, null, 2));
      assert.equal(job.runner, "claude-code");
      assert.ok(job.brief.brief_version >= 2);
      assert.ok(!/✗/.test(job.output), job.output);
      const slug = id.split("/")[1];
      const finished = readdirSync(path.join(vault, "demo", "Backlog", "TASKS")).find((f) => f.endsWith(`_${slug}.md`));
      assert.ok(finished, "the task went to the Backlog");
      assert.match(readFileSync(path.join(vault, "demo", "Backlog", "TASKS", finished), "utf8"), /status: done/);
    } finally {
      await server.stop();
      rmSync(root, { recursive: true, force: true });
    }
  });

  it("with a repository of the area the brief comes for the run's worktree, with its checks", async () => {
    const root = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-real-tasks-repo-"));
    const vault = path.join(root, "vault");
    tasks(vault, ["init-vault", vault, "--area=demo", "--lang=en", "--apply"]);
    const created = tasks(vault, ["new", "demo", "Handed over with a repository", "--kind=task", "--prio=2", "--effort=XS"]);
    const id = created.split("\t")[1];
    // The repository beside the vault, with an origin (git without the person's configuration).
    const gitConfig = path.join(root, "gitconfig");
    writeFileSync(gitConfig, "[user]\n\tname = Test\n\temail = test@example.invalid\n[commit]\n\tgpgsign = false\n");
    const gitEnv = { ...process.env, GIT_CONFIG_GLOBAL: gitConfig, GIT_CONFIG_NOSYSTEM: "1" };
    const git = (cwd, args) => execFileSync("git", args, { cwd, env: gitEnv, encoding: "utf8" });
    const repo = path.join(root, "demo");
    mkdirSync(repo);
    git(repo, ["init", "-q", "-b", "main"]);
    writeFileSync(path.join(repo, "README.md"), "demo\n");
    git(repo, ["add", "README.md"]);
    git(repo, ["commit", "-q", "-m", "initial"]);
    git(root, ["init", "-q", "--bare", "-b", "main", path.join(root, "origin.git")]);
    git(repo, ["remote", "add", "origin", path.join(root, "origin.git")]);
    git(repo, ["push", "-q", "origin", "main"]);
    const configDir = path.join(root, "config");
    mkdirSync(configDir);
    const server = await startServer({
      configDir,
      env: {
        AI_DESKTOP_CLAUDE_CLI: fakeClaude,
        AI_DESKTOP_TASKS_CLI: tasksCli,
        AI_DESKTOP_JOBS_FOREGROUND: "0",
        GIT_CONFIG_GLOBAL: gitConfig,
        GIT_CONFIG_NOSYSTEM: "1",
      },
    });
    try {
      const secret = readFileSync(path.join(server.configDir, "control.secret"), "utf8").trim();
      const res = await server.post(
        "/jobs",
        { source: "tasks-desktop", task_id: id, title: "Handed over", markdown: "## Acceptance\n", vault, area: "demo", mode: "run" },
        { Authorization: `Bearer ${secret}` },
      );
      assert.equal(res.status, 201, res.text);
      let job;
      const deadline = Date.now() + 60000;
      do {
        await new Promise((r) => setTimeout(r, 250));
        job = (await server.get(`/jobs/${res.json.job_id}`)).json;
      } while (job.status === "running" && Date.now() < deadline);
      assert.equal(job.status, "done", JSON.stringify(job, null, 2));
      assert.ok(job.brief.brief_version >= 2);
      // The real engine read the worktree with --repo: a checks section, of one of its sources.
      assert.ok(["testing", "claude-md", "none"].includes(job.brief.checks), JSON.stringify(job.brief));
      assert.match(job.output, /▸ worktree .*job-worktrees/);
      assert.match(job.output, /removed; the branch ai-desktop\/job-[0-9a-f]+-1 is deleted/);
      assert.deepEqual(readdirSync(path.join(configDir, "job-worktrees")), []);
    } finally {
      await server.stop();
      rmSync(root, { recursive: true, force: true });
    }
  });
});
