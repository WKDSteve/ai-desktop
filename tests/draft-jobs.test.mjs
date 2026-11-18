// A job of the kind draft-task (docs/JOBS.md "Drafts"): a free text and the values the vault allows go in, a checked
// draft comes out (`draft` of the job). The area is not known yet, so the job is private: a model that is not local
// waits for a yes. Claude Code (fake_claude) answers once with every tool refused; a local model (a mock) answers the
// same prompt. Nothing starts the real Claude Code.
import assert from "node:assert/strict";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { after, before, describe, it } from "node:test";

import { binaryPath, startServer } from "./helpers/server.mjs";

const exe = process.platform === "win32" ? ".exe" : "";
const fakeClaude = path.join(path.dirname(binaryPath()), `fake_claude${exe}`);

function lines(file) {
  return existsSync(file) ? readFileSync(file, "utf8").trim().split("\n").filter(Boolean).map((l) => JSON.parse(l)) : [];
}

// An OpenAI-compatible local model that answers with what `reply()` gives and records the prompt.
function startMock(reply) {
  const seen = [];
  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => {
      seen.push(JSON.parse(Buffer.concat(chunks).toString("utf8") || "{}"));
      res.writeHead(200, { "Content-Type": "text/event-stream" });
      res.write(`data: ${JSON.stringify({ choices: [{ delta: { content: reply() } }] })}\n\n`);
      res.end("data: [DONE]\n\n");
    });
  });
  return new Promise((resolve) =>
    server.listen(0, "127.0.0.1", () => resolve({ port: server.address().port, seen, close: () => new Promise((r) => server.close(r)) })),
  );
}

describe("a job of the kind draft-task", { skip: !existsSync(fakeClaude) && "build the test doubles first" }, () => {
  let root;
  let server;
  let mock;
  let answer = "";
  const claudeLog = () => path.join(root, "claude.log");

  before(async () => {
    root = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-draft-"));
    mock = await startMock(() => answer);
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
      env: { AI_DESKTOP_CLAUDE_CLI: fakeClaude, AI_DESKTOP_JOBS_PER_MINUTE: "1000", FAKE_CLAUDE_LOG: claudeLog() },
    });
  });
  after(async () => {
    await server?.stop();
    await mock?.close();
    rmSync(root, { recursive: true, force: true });
  });

  const secret = () => readFileSync(path.join(server.configDir, "control.secret"), "utf8").trim();

  function body(extra = {}) {
    return {
      source: "tasks-desktop",
      task_id: "draft/new",
      title: "Neue Task",
      markdown: "Der Knopf im Formular soll blau sein, sonst sieht ihn keiner.",
      vault: path.join(root, "tasks-vault"),
      area: "",
      mode: "run",
      kind: "draft-task",
      choices: { areas: ["demo", "other"], kinds: ["task", "bug"], tags: ["ui"], categories: ["ux"], plans: [] },
      ...extra,
    };
  }

  async function until(id) {
    const deadline = Date.now() + 15000;
    for (;;) {
      const job = (await server.get(`/jobs/${id}`)).json;
      if (job.status !== "running" || Date.now() > deadline) return job;
      await new Promise((r) => setTimeout(r, 100));
    }
  }

  it("refuses a draft without choices, and choices on another kind", async () => {
    const auth = { Authorization: `Bearer ${secret()}` };
    const none = body();
    delete none.choices;
    assert.equal((await server.post("/jobs", none, auth)).status, 400);
    assert.equal((await server.post("/jobs", body({ kind: "handoff", area: "demo" }), auth)).status, 400);
    assert.equal((await server.post("/jobs", body({ choices: { areas: [], kinds: ["task"] } }), auth)).status, 400);
  });

  it("counts as private: Claude Code waits for a yes, then answers once without tools", async () => {
    const res = await server.post("/jobs", body(), { Authorization: `Bearer ${secret()}` });
    assert.equal(res.status, 201, res.text);
    assert.equal(res.json.private, true);
    assert.equal(res.json.status, "needs_confirmation");
    const blind = await server.post(`/jobs/${res.json.job_id}/run`, {}, { Authorization: `Bearer ${secret()}` });
    assert.equal(blind.status, 409);
    const yes = await server.post(`/jobs/${res.json.job_id}/run`, { confirm_private: true }, { Authorization: `Bearer ${secret()}` });
    assert.equal(yes.status, 200, yes.text);
    const job = await until(res.json.job_id);
    assert.equal(job.status, "done", JSON.stringify(job));
    assert.equal(job.runner, "claude-code");
    assert.match(job.note, /draft: one answer, no tools/);
    assert.equal(job.draft.schema, "tasks-draft/1");
    assert.equal(job.draft.title, "Knopf blau machen");
    assert.equal(job.draft.area, "demo");
    assert.deepEqual(job.draft.tags, ["ui"]);
    assert.deepEqual(job.draft.dropped, ["tags: made-up"]);
    assert.deepEqual(job.draft.acceptance, ["Der Knopf ist blau"]);
    assert.equal(job.draft_error, undefined);

    const call = lines(claudeLog()).at(-1);
    assert.equal(path.basename(call.cwd), "draft-work", "an empty folder of its own: no CLAUDE.md of a project");
    assert.ok(!call.args.includes("--allowedTools") && !call.args.includes("--add-dir"));
    for (const tool of ["Bash", "Read", "Write", "WebFetch"]) assert.ok(call.args.includes(tool), tool);
    assert.match(call.prompt, /- area: demo, other/);
    assert.match(call.prompt, /~~~~text\nDer Knopf im Formular soll blau sein/);
  });

  it("a local model runs at once; its draft is checked against the choices", async () => {
    answer =
      'Hier:\n<!-- draft:begin -->\n{"title": "Blauer Knopf", "area": "nowhere", "kind": "bug", "prio": 1, ' +
      '"effort": "M", "value": 4, "tags": [], "category": "ux", "summary": "s", "acceptance": ["a", "b"]}\n<!-- draft:end -->';
    const res = await server.post("/jobs", body({ model: "mock-local" }), { Authorization: `Bearer ${secret()}` });
    assert.equal(res.status, 201, res.text);
    assert.equal(res.json.status, "running");
    const job = await until(res.json.job_id);
    assert.equal(job.status, "done", JSON.stringify(job));
    assert.equal(job.runner, "model");
    assert.equal(job.draft.area, undefined, "an area that is not in the vault is left out");
    assert.deepEqual(job.draft.dropped, ["area: nowhere"]);
    assert.equal(job.draft.kind, "bug");
    assert.deepEqual(job.draft.category, ["ux"]);
    const asked = mock.seen.at(-1).messages.map((m) => m.content).join("\n");
    assert.match(asked, /only describe it/, "the system text of a draft");
  });

  it("an answer without a valid block gives draft_error and no draft", async () => {
    answer = "I cannot help with that.";
    const res = await server.post("/jobs", body({ model: "mock-local" }), { Authorization: `Bearer ${secret()}` });
    const job = await until(res.json.job_id);
    assert.equal(job.status, "done");
    assert.equal(job.draft, undefined);
    assert.match(job.draft_error, /no draft block/);
    answer = '<!-- draft:begin -->\n{"title": 5}\n<!-- draft:end -->';
    const bad = await until((await server.post("/jobs", body({ model: "mock-local" }), { Authorization: `Bearer ${secret()}` })).json.job_id);
    assert.equal(bad.draft, undefined);
    assert.match(bad.draft_error, /'title' must be a string/);
  });
});
