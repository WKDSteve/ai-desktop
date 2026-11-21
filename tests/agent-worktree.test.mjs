// A hand-over in a repository works in a git worktree of its own, made from origin/main (docs/JOBS.md, "Runs"),
// never in the shared checkout where other sessions keep uncommitted work. Real git against a throw-away origin; the
// agent is tests/doubles/fake_claude, which stages everything (`git add -A`) as a careless agent would.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { describe, it } from "node:test";

import { binaryPath, startServer } from "./helpers/server.mjs";

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

// A vault, beside it the shared checkout `demo` of a bare origin, with a change of another session in it (a tracked
// file changed, an untracked file added); git without the person's configuration (no signing, a test identity).
function world({ origin = true } = {}) {
  const root = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-wt-"));
  const gitConfig = path.join(root, "gitconfig");
  writeFileSync(gitConfig, "[user]\n\tname = Test\n\temail = test@example.invalid\n[commit]\n\tgpgsign = false\n");
  const gitEnv = { ...process.env, GIT_CONFIG_GLOBAL: gitConfig, GIT_CONFIG_NOSYSTEM: "1" };
  const git = (cwd, args) => execFileSync("git", args, { cwd, env: gitEnv, encoding: "utf8" }).trim();
  const vault = path.join(root, "tasks-vault");
  mkdirSync(vault);
  const checkout = path.join(root, "demo");
  mkdirSync(checkout);
  git(checkout, ["init", "-q", "-b", "main"]);
  writeFileSync(path.join(checkout, "README.md"), "the readme\n");
  git(checkout, ["add", "README.md"]);
  git(checkout, ["commit", "-q", "-m", "initial"]);
  const bare = path.join(root, "origin.git");
  if (origin) {
    git(root, ["init", "-q", "--bare", "-b", "main", bare]);
    git(checkout, ["remote", "add", "origin", bare]);
    git(checkout, ["push", "-q", "origin", "main"]);
  }
  writeFileSync(path.join(checkout, "README.md"), "the readme, half changed by another session\n");
  writeFileSync(path.join(checkout, "foreign.txt"), "not the agent's\n");
  const bin = path.join(root, "bin");
  mkdirSync(bin);
  const tasks = path.join(bin, `tasks${exe}`);
  copyFileSync(fakeTasks, tasks);
  const configDir = path.join(root, "config");
  mkdirSync(configDir);
  writeFileSync(path.join(configDir, "config.json"), JSON.stringify({ version: 1, settings: {} }));
  return {
    root, vault, checkout, bare, configDir, tasks, git, gitEnv,
    worktrees: path.join(configDir, "job-worktrees"),
    claudeLog: path.join(root, "claude.log"),
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
      AI_DESKTOP_CLAUDE_CLI: fakeClaude,
      AI_DESKTOP_TASKS_CLI: w.tasks,
      AI_DESKTOP_JOBS_PER_MINUTE: "1000",
      FAKE_TASKS_LOG: path.join(w.root, "tasks.log"),
      FAKE_CLAUDE_LOG: w.claudeLog,
      GIT_CONFIG_GLOBAL: w.gitEnv.GIT_CONFIG_GLOBAL,
      GIT_CONFIG_NOSYSTEM: "1",
      ...env,
    },
  });
}

function auth(server) {
  const secret = readFileSync(path.join(server.configDir, "control.secret"), "utf8").trim();
  return { Authorization: `Bearer ${secret}` };
}

async function waitFor(server, id, done, ms = 20000) {
  const deadline = Date.now() + ms;
  for (;;) {
    const job = (await server.get(`/jobs/${id}`)).json;
    if (done(job) || Date.now() > deadline) return job;
    await new Promise((r) => setTimeout(r, 100));
  }
}

async function until(check, ms = 10000) {
  const deadline = Date.now() + ms;
  while (!check() && Date.now() < deadline) await new Promise((r) => setTimeout(r, 100));
  return check();
}

function branches(w) {
  return w.git(w.checkout, ["branch", "--list", "ai-desktop/*", "--format=%(refname:short)"]).split("\n").filter(Boolean);
}

// The shared checkout as another session left it: still on main at its commit, the foreign change still there.
function assertCheckoutUntouched(w, head) {
  assert.equal(w.git(w.checkout, ["rev-parse", "HEAD"]), head, "the checkout's HEAD did not move");
  assert.equal(w.git(w.checkout, ["branch", "--show-current"]), "main");
  assert.equal(readFileSync(path.join(w.checkout, "README.md"), "utf8"), "the readme, half changed by another session\n");
  assert.ok(existsSync(path.join(w.checkout, "foreign.txt")));
  assert.ok(!existsSync(path.join(w.checkout, "agent.txt")), "the agent never wrote into the checkout");
}

describe("the worktree of a hand-over", { skip: !existsSync(fakeClaude) && "build the test doubles first" }, () => {
  it("the agent commits in its own worktree: a foreign uncommitted change stays out of its commit", async () => {
    const w = world();
    const server = await serverFor(w, { FAKE_CLAUDE_COMMIT: "1" });
    try {
      const head = w.git(w.checkout, ["rev-parse", "HEAD"]);
      const res = await server.post("/jobs", { ...TASK, vault: w.vault }, auth(server));
      assert.equal(res.status, 201, res.text);
      const id = res.json.job_id;
      const job = await waitFor(server, id, (j) => j.status !== "running");
      assert.equal(job.status, "done", JSON.stringify(job));

      const run = lines(w.claudeLog).at(-1);
      const dir = path.join(w.worktrees, `job-${id}-1`);
      assert.equal(path.resolve(run.cwd), path.resolve(dir), "the agent ran in the worktree");
      assert.equal(path.resolve(run.args[run.args.indexOf("--add-dir") + 1]), path.resolve(w.vault));
      assert.match(run.prompt, new RegExp(`branch ai-desktop/job-${id}-1`));
      assert.match(run.prompt, /`git push origin HEAD:main`/);

      // What reached origin: the agent's file, on top of the commit of main, and nothing of the other session.
      const files = w.git(w.bare, ["ls-tree", "-r", "--name-only", "main"]).split("\n").sort();
      assert.deepEqual(files, ["README.md", "agent.txt"]);
      assert.equal(w.git(w.bare, ["show", "main:README.md"]), "the readme");
      assert.equal(w.git(w.bare, ["log", "-1", "--format=%s", "main"]), "feat: the work of the agent");
      assertCheckoutUntouched(w, head);

      // Gone after the run: the folder, the record, the branch (its commit is on origin).
      assert.ok(!existsSync(dir), `the worktree is removed: ${job.output}`);
      assert.deepEqual(readdirSync(w.worktrees), [], "no record left");
      assert.deepEqual(branches(w), []);
      assert.equal(w.git(w.checkout, ["worktree", "list", "--porcelain"]).match(/^worktree /gm).length, 1);
      assert.match(job.output, /▸ worktree .* removed; the branch ai-desktop\/job-[0-9a-f]+-1 is deleted/);
    } finally {
      await server.stop();
      w.cleanup();
    }
  });

  it("a commit that was not pushed stays on the job's branch", async () => {
    const w = world();
    const server = await serverFor(w, { FAKE_CLAUDE_COMMIT: "1", FAKE_CLAUDE_NO_PUSH: "1" });
    try {
      const head = w.git(w.checkout, ["rev-parse", "HEAD"]);
      const res = await server.post("/jobs", { ...TASK, vault: w.vault }, auth(server));
      const id = res.json.job_id;
      const job = await waitFor(server, id, (j) => j.status !== "running");
      assert.equal(job.status, "done", JSON.stringify(job));
      const branch = `ai-desktop/job-${id}-1`;
      assert.deepEqual(branches(w), [branch]);
      assert.equal(w.git(w.checkout, ["log", "-1", "--format=%s", branch]), "feat: the work of the agent");
      assert.deepEqual(w.git(w.checkout, ["ls-tree", "-r", "--name-only", branch]).split("\n").sort(), ["README.md", "agent.txt"]);
      assert.ok(!existsSync(path.join(w.worktrees, `job-${id}-1`)), "the folder goes all the same");
      assert.match(job.output, new RegExp(`1 commit\\(s\\) on no remote stay on the branch ${branch}`));
      assert.equal(w.git(w.bare, ["log", "-1", "--format=%s", "main"]), "initial", "nothing pushed");
      assertCheckoutUntouched(w, head);
    } finally {
      await server.stop();
      w.cleanup();
    }
  });

  it("a cancel removes the worktree once the agent's process tree is gone", async () => {
    const w = world();
    const server = await serverFor(w, { FAKE_CLAUDE_SLEEP_MS: "30000" });
    try {
      const res = await server.post("/jobs", { ...TASK, vault: w.vault }, auth(server));
      const id = res.json.job_id;
      await waitFor(server, id, (j) => /tasks sleep/.test(j.output || ""));
      const dir = path.join(w.worktrees, `job-${id}-1`);
      assert.ok(existsSync(dir), "the worktree is there while the agent runs");
      assert.equal((await server.post(`/jobs/${id}/cancel`, {}, auth(server))).status, 200);
      assert.ok(await until(() => !existsSync(dir)), "the worktree is removed after the cancel");
      assert.ok(await until(() => branches(w).length === 0), "its branch had nothing of its own: deleted");
    } finally {
      await server.stop();
      w.cleanup();
    }
  });

  it("what a killed ai-desktop left behind, the next start removes; an unpushed commit keeps its branch", async () => {
    const w = world();
    let server = await serverFor(w, { FAKE_CLAUDE_SLEEP_MS: "30000" });
    let id;
    try {
      const res = await server.post("/jobs", { ...TASK, vault: w.vault }, auth(server));
      id = res.json.job_id;
      await waitFor(server, id, (j) => /tasks sleep/.test(j.output || ""));
    } finally {
      await server.stop(); // a hard kill: no cleanup of its own
    }
    const dir = path.join(w.worktrees, `job-${id}-1`);
    const branch = `ai-desktop/job-${id}-1`;
    assert.ok(existsSync(dir), "the kill left the worktree");
    // A commit the agent made before the end, on no remote.
    writeFileSync(path.join(dir, "late.txt"), "late\n");
    w.git(dir, ["add", "late.txt"]);
    w.git(dir, ["commit", "-q", "-m", "late work"]);
    server = await serverFor(w);
    try {
      assert.ok(await until(() => !existsSync(dir)), "the next start removed it");
      assert.ok(await until(() => readdirSync(w.worktrees).length === 0), "and its record");
      assert.deepEqual(branches(w), [branch]);
      assert.equal(w.git(w.checkout, ["log", "-1", "--format=%s", branch]), "late work");
    } finally {
      await server.stop();
      w.cleanup();
    }
  });

  it("a record that names a folder outside the worktrees is left alone, the folder kept", async () => {
    const w = world();
    // A folder beside the worktrees and a record that would have the sweep remove it (by name it passes as one).
    const victim = path.join(w.configDir, "zz-victim");
    mkdirSync(path.join(victim, "inner"), { recursive: true });
    writeFileSync(path.join(victim, "inner", "keep.txt"), "keep\n");
    mkdirSync(w.worktrees);
    const record = path.join(w.worktrees, "zz-victim.json");
    writeFileSync(record, JSON.stringify({ repo: w.checkout, dir: victim, branch: "ai-desktop/zz-victim" }));
    const server = await serverFor(w);
    try {
      assert.equal(await until(() => !existsSync(victim), 3000), false, "the folder outside is kept");
      assert.ok(existsSync(path.join(victim, "inner", "keep.txt")));
      assert.ok(existsSync(record), "the record stays for the person to look at");
      assertCheckoutUntouched(w, w.git(w.checkout, ["rev-parse", "HEAD"]));
    } finally {
      await server.stop();
      w.cleanup();
    }
  });

  it("a repository without origin/main: the agent does not run in the shared checkout", async () => {
    const w = world({ origin: false });
    const server = await serverFor(w);
    try {
      const res = await server.post("/jobs", { ...TASK, vault: w.vault }, auth(server));
      const job = await waitFor(server, res.json.job_id, (j) => j.status !== "running");
      assert.equal(job.status, "failed");
      assert.match(job.error, /no worktree of its own .* has no origin\/main/);
      assert.equal(lines(w.claudeLog).length, 0, "Claude Code did not run");
      assert.ok(!existsSync(w.worktrees) || readdirSync(w.worktrees).length === 0, "no record left");
    } finally {
      await server.stop();
      w.cleanup();
    }
  });
});
