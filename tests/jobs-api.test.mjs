// POST /jobs and the routes of the dashboard around it (docs/JOBS.md), against the real binary: a job only with the
// control secret, never from a web page, bounded; prefill and run; a private area goes to a local model only.
import assert from "node:assert/strict";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { after, before, describe, it } from "node:test";

import { NO_AUTH, request, runToExit, startServer } from "./helpers/server.mjs";

const TASK = {
  source: "tasks-desktop",
  task_id: "ai-desktop/auftrag-von-aussen",
  title: "Auftrag von außen annehmen",
  markdown: "---\ntitle: Auftrag\n---\n\n## Akzeptanz\n\n- [ ] eins\n",
  vault: "E:/repos/tasks-vault",
  area: "ai-desktop",
  mode: "prefill",
};

function secretOf(server) {
  return readFileSync(path.join(server.configDir, "control.secret"), "utf8").trim();
}

function auth(server) {
  return { Authorization: `Bearer ${secretOf(server)}` };
}

async function waitFor(server, id, done) {
  const deadline = Date.now() + 10000;
  for (;;) {
    const res = await server.get(`/jobs/${id}`);
    if (done(res.json) || Date.now() > deadline) return res.json;
    await new Promise((r) => setTimeout(r, 100));
  }
}

// A local OpenAI-compatible model: streams "Hel" "lo" and records what it was asked.
function startMock() {
  const seen = [];
  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => {
      seen.push(JSON.parse(Buffer.concat(chunks).toString("utf8") || "{}"));
      res.writeHead(200, { "Content-Type": "text/event-stream" });
      for (const piece of ["Hel", "lo"]) {
        res.write(`data: ${JSON.stringify({ choices: [{ delta: { content: piece } }] })}\n\n`);
      }
      res.end("data: [DONE]\n\n");
    });
  });
  return new Promise((resolve) =>
    server.listen(0, "127.0.0.1", () =>
      resolve({ port: server.address().port, seen, close: () => new Promise((r) => server.close(r)) }),
    ),
  );
}

describe("POST /jobs", () => {
  let server;
  let mock;
  let configDir;

  before(async () => {
    mock = await startMock();
    configDir = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-jobs-"));
    writeFileSync(
      path.join(configDir, "config.json"),
      JSON.stringify({
        version: 1,
        settings: { private_areas: "tricentis-vault" },
        custom_models: [
          { id: "mock-local", provider: "openai-compatible", base_url: `http://127.0.0.1:${mock.port}/v1` },
        ],
      }),
    );
    server = await startServer({ configDir, env: { AI_DESKTOP_JOBS_PER_MINUTE: "1000" } });
  });
  after(async () => {
    await server?.stop();
    await mock?.close();
    rmSync(configDir, { recursive: true, force: true });
  });

  it("writes a control secret of 64 hex digits at start", () => {
    assert.match(secretOf(server), /^[0-9a-f]{64}$/);
  });

  it("takes a prefill job with the secret and keeps it in the dashboard, nothing runs", async () => {
    const res = await server.post("/jobs", TASK, auth(server));
    assert.equal(res.status, 201, res.text);
    assert.match(res.json.job_id, /^[0-9a-f]{16}$/);
    assert.equal(res.json.status, "prefilled");
    assert.equal(res.json.private, false);
    assert.equal(res.headers["access-control-allow-origin"], undefined);

    const job = await server.get(`/jobs/${res.json.job_id}`);
    assert.equal(job.status, 200);
    assert.equal(job.json.markdown, TASK.markdown);
    assert.equal(job.json.title, TASK.title);
    assert.equal(job.json.kind, "handoff");
    assert.equal(job.json.output, "");

    const list = await server.get("/jobs");
    assert.ok(list.json.jobs.some((j) => j.id === res.json.job_id));
    assert.equal(list.json.jobs[0].markdown, undefined, "the list carries no text");
    assert.equal(list.json.enabled, true);
  });

  it("refuses a job without the secret, or with a wrong one", async () => {
    for (const headers of [NO_AUTH, { Authorization: "Bearer 00" }, { Authorization: `bearer ${secretOf(server)}` },
      { Authorization: `Bearer ${"0".repeat(64)}` }]) {
      const res = await server.post("/jobs", TASK, headers);
      assert.equal(res.status, 401, JSON.stringify(headers));
    }
  });

  it("refuses a job from a web page, even with the secret, the dashboard's own origin included", async () => {
    for (const origin of [`http://127.0.0.1:${server.port}`, "https://evil.example"]) {
      const res = await server.post("/jobs", TASK, { ...auth(server), Origin: origin });
      assert.equal(res.status, 403, origin);
    }
    for (const header of ["Sec-Fetch-Site", "Sec-Fetch-Mode"]) {
      const res = await server.post("/jobs", TASK, { ...auth(server), [header]: "cross-site" });
      assert.equal(res.status, 403, header);
    }
  });

  it("answers a preflight without CORS", async () => {
    const res = await request(server.port, {
      method: "OPTIONS",
      pathname: "/jobs",
      headers: { Origin: "https://evil.example", "Access-Control-Request-Method": "POST" },
    });
    assert.equal(res.headers["access-control-allow-origin"], undefined);
    assert.notEqual(res.status, 204);
  });

  it("refuses a foreign Host (DNS rebinding) on every route of the jobs", async () => {
    const res = await server.post("/jobs", TASK, { ...auth(server), Host: `rebind.example:${server.port}` });
    assert.equal(res.status, 403);
    assert.equal((await server.get("/jobs", { Host: `rebind.example:${server.port}` })).status, 403);
    assert.equal((await server.get("/jobs", { Host: `localhost:${server.port}` })).status, 200);
  });

  it("wants JSON and bounds the body and the fields", async () => {
    const plain = await server.post("/jobs", JSON.stringify(TASK), { ...auth(server), "Content-Type": "text/plain" });
    assert.equal(plain.status, 415);
    const big = await server.post("/jobs", { ...TASK, markdown: "x".repeat(330 * 1024) }, auth(server));
    assert.equal(big.status, 413);
    for (const bad of [
      { ...TASK, mode: "execute" },
      { ...TASK, kind: "delete-everything" },
      { ...TASK, task_id: "../../etc" },
      { ...TASK, title: "two\nlines" },
      { ...TASK, markdown: "bidi \u202e override" },
      { ...TASK, markdown: "escape \u001b[31m" },
      { ...TASK, extra: 1 },
      (({ vault, ...rest }) => rest)(TASK),
    ]) {
      const res = await server.post("/jobs", bad, auth(server));
      assert.equal(res.status, 400, JSON.stringify(bad).slice(0, 80));
    }
  });

  it("stores every kind and shows it", async () => {
    for (const kind of ["handoff", "review-done", "draft-task", "prepare-decision", "split-plan"]) {
      // A draft carries the values of the vault (tests/draft-jobs.test.mjs).
      const choices = kind === "draft-task" ? { choices: { areas: ["a"], kinds: ["task"] } } : {};
      const res = await server.post("/jobs", { ...TASK, kind, ...choices }, auth(server));
      assert.equal(res.status, 201, kind);
      assert.equal((await server.get(`/jobs/${res.json.job_id}`)).json.kind, kind);
    }
  });

  it("runs a job at once with mode run, on the model it names", async () => {
    const res = await server.post("/jobs", { ...TASK, mode: "run", model: "mock-local" }, auth(server));
    assert.equal(res.status, 201, res.text);
    const job = await waitFor(server, res.json.job_id, (j) => j.status !== "running");
    assert.equal(job.status, "done", JSON.stringify(job));
    assert.equal(job.output, "Hello");
    assert.equal(job.model, "mock-local");
    const asked = mock.seen.at(-1);
    assert.ok(asked.messages.some((m) => m.content.includes("## Akzeptanz")));
    assert.ok(asked.messages.some((m) => m.content.includes(TASK.task_id)));
  });

  it("holds a private area for the person when the model is not local, and runs it on a local one", async () => {
    const cloud = await server.post("/jobs", { ...TASK, area: "tricentis-vault", mode: "run", model: "gpt-4o" }, auth(server));
    assert.equal(cloud.status, 201);
    assert.equal(cloud.json.private, true);
    assert.equal(cloud.json.status, "needs_confirmation");
    assert.match(cloud.json.note, /private/);

    const hinted = await server.post("/jobs", { ...TASK, private: true, mode: "run", model: "claude-haiku-5-5" }, auth(server));
    assert.equal(hinted.json.status, "needs_confirmation");

    const local = await server.post("/jobs", { ...TASK, area: "tricentis-vault", mode: "run", model: "mock-local" }, auth(server));
    assert.equal(local.json.status, "running");
    assert.equal((await waitFor(server, local.json.job_id, (j) => j.status !== "running")).status, "done");
  });

  it("the dashboard runs a job with its edited text; a private one only after a yes", async () => {
    const origin = await server.session();
    const created = await server.post("/jobs", { ...TASK, area: "tricentis-vault" }, auth(server));
    const id = created.json.job_id;

    const blind = await server.post(`/jobs/${id}/run`, { model: "mock-local" }, NO_AUTH);
    assert.equal(blind.status, 401, "neither the dashboard nor the secret");
    const forged = await server.post(`/jobs/${id}/run`, { model: "gpt-4o", confirm_private: true },
      { ...NO_AUTH, Origin: `http://127.0.0.1:${server.port}` });
    assert.equal(forged.status, 401, "the dashboard's Origin alone proves nothing");

    const held = await server.post(`/jobs/${id}/run`, { model: "gpt-4o" }, origin);
    assert.equal(held.status, 409);
    assert.equal(held.json.status, "needs_confirmation");

    const ran = await server.post(`/jobs/${id}/run`, { model: "mock-local", markdown: "edited text" }, origin);
    assert.equal(ran.status, 200, ran.text);
    const job = await waitFor(server, id, (j) => j.status !== "running");
    assert.equal(job.markdown, "edited text");
    assert.ok(mock.seen.at(-1).messages.some((m) => m.content.includes("edited text")));

    // With the yes the job goes to the model it names; gpt-4o has no key here, so the run fails, but it ran.
    const confirmed = await server.post(`/jobs/${id}/run`, { model: "gpt-4o", confirm_private: true }, origin);
    assert.equal(confirmed.status, 200, confirmed.text);
    const failed = await waitFor(server, id, (j) => j.status !== "running");
    assert.equal(failed.status, "failed");
    assert.ok(failed.error);

    const bad = await server.post(`/jobs/${id}/run`, { markdown: "\u200b" }, origin);
    assert.equal(bad.status, 400);
  });

  it("a job is put away from the dashboard or with the secret", async () => {
    const created = await server.post("/jobs", TASK, auth(server));
    const id = created.json.job_id;
    assert.equal((await server.del(`/jobs/${id}`, NO_AUTH)).status, 401);
    assert.equal((await server.del(`/jobs/${id}`, auth(server))).status, 204);
    assert.equal((await server.get(`/jobs/${id}`)).status, 404);
    assert.equal((await server.del(`/jobs/${id}`, auth(server))).status, 404);
    assert.equal((await server.get("/jobs/not-an-id")).status, 404);
  });

  it("is refused when the setting jobs is off", async () => {
    const put = await server.put("/settings", { jobs: "off" });
    assert.equal(put.status, 200, put.text);
    const res = await server.post("/jobs", TASK, auth(server));
    assert.equal(res.status, 403);
    assert.equal((await server.get("/jobs")).json.enabled, false);
    await server.put("/settings", { jobs: null });
    assert.equal((await server.post("/jobs", TASK, auth(server))).status, 201);
  });
});

describe("POST /jobs, rate and lifetime of the secret", () => {
  it("takes at most 10 jobs a minute and says when to come back; a request without the secret is not counted", async () => {
    const server = await startServer();
    try {
      for (let i = 0; i < 12; i += 1) {
        assert.equal((await server.post("/jobs", TASK, NO_AUTH)).status, 401);
      }
      const statuses = [];
      let last;
      for (let i = 0; i < 12; i += 1) {
        last = await server.post("/jobs", TASK, auth(server));
        statuses.push(last.status);
      }
      assert.deepEqual(statuses.slice(0, 10), Array(10).fill(201));
      assert.equal(last.status, 429);
      assert.ok(Number(last.headers["retry-after"]) >= 1);
    } finally {
      await server.stop();
    }
  });

  it("a new secret at every start; the old one no longer works", async () => {
    const configDir = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-jobs-secret-"));
    try {
      const first = await startServer({ configDir });
      const old = secretOf(first);
      await first.stop();
      const second = await startServer({ configDir });
      try {
        assert.notEqual(secretOf(second), old);
        assert.equal((await second.post("/jobs", TASK, { Authorization: `Bearer ${old}` })).status, 401);
      } finally {
        await second.stop();
      }
    } finally {
      rmSync(configDir, { recursive: true, force: true });
    }
  });

  it("a second start on a taken port leaves the secret of the running server alone", async () => {
    const server = await startServer();
    try {
      const before = secretOf(server);
      const second = await runToExit(["--port", String(server.port)], {
        AI_DESKTOP_CONFIG_DIR: server.configDir,
        AI_DESKTOP_JOBS_FOREGROUND: "0",
      });
      assert.equal(second.code, 1, second.output);
      assert.equal(secretOf(server), before);
      assert.equal((await server.post("/jobs", TASK, auth(server))).status, 201);
    } finally {
      await server.stop();
    }
  });

  it("without a secret in force POST /jobs answers 503", async () => {
    // The config dir is a file, so the secret cannot be written there; the server runs on.
    const base = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-jobs-nodir-"));
    const notADir = path.join(base, "file");
    writeFileSync(notADir, "x");
    try {
      const server = await startServer({ configDir: notADir });
      try {
        const res = await server.post("/jobs", TASK, { Authorization: `Bearer ${"a".repeat(64)}` });
        assert.equal(res.status, 503);
        assert.ok(!existsSync(path.join(notADir, "control.secret")));
      } finally {
        await server.stop();
      }
    } finally {
      rmSync(base, { recursive: true, force: true });
    }
  });
});
