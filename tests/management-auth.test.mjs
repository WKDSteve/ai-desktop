// The login of the management API (src/session.hpp, docs/JOBS.md "Threat model"): 127.0.0.1 is reachable by every
// account of the machine and a program can send any Origin, so the routes with an effect, and those that read jobs or
// settings, want the control secret or a session token of the dashboard.
import assert from "node:assert/strict";
import { after, before, describe, it } from "node:test";

import { NO_AUTH, request, startServer } from "./helpers/server.mjs";

const TASK = {
  source: "tasks-desktop",
  task_id: "demo/first-task",
  title: "First task",
  markdown: "---\ntitle: First task\n---\n",
  area: "demo",
  vault: "E:/repos/tasks-vault",
  mode: "prefill",
};

describe("the management API wants a proof", () => {
  let server;
  let jobId;
  before(async () => {
    server = await startServer();
    const created = await server.post("/jobs", TASK);
    assert.equal(created.status, 201, created.text);
    jobId = created.json.job_id;
  });
  after(async () => {
    await server?.stop();
  });

  const routes = () => [
    ["GET", `/jobs/${jobId}`, undefined],
    ["POST", `/jobs/${jobId}/run`, { model: "mock-local" }],
    ["POST", `/jobs/${jobId}/cancel`, {}],
    ["DELETE", `/jobs/${jobId}`, undefined],
    ["GET", "/jobs", undefined],
    ["GET", "/settings", undefined],
    ["PUT", "/settings", { claude_cli: "C:\\evil\\claude.exe" }],
    ["GET", "/keys", undefined],
    ["PUT", "/keys/openai", { value: "sk-stolen" }],
    ["DELETE", "/keys/openai", undefined],
    ["GET", "/models/custom", undefined],
    ["POST", "/models/custom", { id: "evil", provider: "openai", base_url: "http://127.0.0.1:9" }],
    ["GET", "/usage", undefined],
    ["GET", "/events", undefined],
    ["POST", "/decision", { id: "1", answer: "allow" }],
  ];

  it("refuses every such route without a proof, and with the dashboard's Origin alone", async () => {
    const forged = { ...NO_AUTH, Origin: `http://127.0.0.1:${server.port}` };
    for (const [method, pathname, body] of routes()) {
      for (const headers of [NO_AUTH, forged, { Authorization: "Bearer " + "0".repeat(64) }]) {
        const res = await request(server.port, { method, pathname, body, headers: { ...headers } });
        assert.equal(res.status, 401, `${method} ${pathname} ${JSON.stringify(headers)}`);
      }
    }
    // Nothing of it happened: the job is still there, the setting unchanged.
    assert.equal((await server.get(`/jobs/${jobId}`)).json.status, "prefilled");
    assert.notEqual((await server.get("/settings")).json.settings.claude_cli?.value, "C:\\evil\\claude.exe");
  });

  it("takes the control secret and the dashboard's session token", async () => {
    const session = await server.session();
    for (const headers of [{}, session]) {
      assert.equal((await server.get("/settings", headers)).status, 200);
      assert.equal((await server.get("/jobs", headers)).status, 200);
      assert.equal((await server.get("/usage", headers)).status, 200);
      assert.equal((await server.put("/settings", { private_areas: "secret" }, headers)).status, 200);
    }
    const cancel = await server.post(`/jobs/${jobId}/cancel`, {}, session);
    assert.notEqual(cancel.status, 401, cancel.text);
  });

  it("leaves the ai.nvim contract, the model list and the files of the dashboard open", async () => {
    for (const pathname of ["/", "/app.js", "/health", "/models"]) {
      assert.equal((await server.get(pathname, NO_AUTH)).status, 200, pathname);
    }
    const ask = await server.post("/ask", { prompt: "hi", model: "nope" }, NO_AUTH);
    assert.notEqual(ask.status, 401);
  });

  it("reads a job to the control secret, as tasks-desktop and tasks.nvim send it", async () => {
    assert.equal((await server.get(`/jobs/${jobId}`)).status, 200);
  });
});

describe("the login of the dashboard", () => {
  let server;
  before(async () => {
    server = await startServer();
  });
  after(async () => {
    await server?.stop();
  });

  const ticketOf = async () => new URL(await server.dashboardUrl()).hash.match(/ticket=([0-9a-f]{64})/)[1];

  it("trades a ticket for a token once", async () => {
    const ticket = await ticketOf();
    const first = await server.post("/session", { ticket }, NO_AUTH);
    assert.equal(first.status, 200, first.text);
    assert.match(first.json.token, /^[0-9a-f]{64}$/);
    assert.equal(first.headers["cache-control"], "no-store");
    assert.equal((await server.get("/settings", { Authorization: `Bearer ${first.json.token}` })).status, 200);
    assert.equal((await server.post("/session", { ticket }, NO_AUTH)).status, 401, "a ticket is used up");
    assert.equal((await server.get("/settings", { Authorization: `Bearer ${ticket}` })).status, 401, "no token");
  });

  it("refuses unknown tickets, and tickets for a program without the secret", async () => {
    for (const body of [{ ticket: "0".repeat(64) }, { ticket: "" }, {}, { ticket: 7 }]) {
      assert.equal((await server.post("/session", body, NO_AUTH)).status, 401, JSON.stringify(body));
    }
    assert.equal((await server.post("/session/ticket", {}, NO_AUTH)).status, 401);
    assert.equal(
      (await server.post("/session/ticket", {}, { ...NO_AUTH, Origin: `http://127.0.0.1:${server.port}` })).status,
      401,
    );
  });

  it("prints no ticket when stdout is no console", () => {
    assert.match(server.output(), new RegExp(`Dashboard: http://127\\.0\\.0\\.1:${server.port}/\\r?\\n`));
    assert.doesNotMatch(server.output(), /ticket=/);
  });
});

describe("a session ends with the server", () => {
  it("a token of the last start no longer works", async () => {
    const first = await startServer();
    const session = await first.session();
    const port = first.port;
    await first.stop();
    const second = await startServer({ port });
    try {
      assert.equal((await second.get("/settings", session)).status, 401);
    } finally {
      await second.stop();
    }
  });
});
