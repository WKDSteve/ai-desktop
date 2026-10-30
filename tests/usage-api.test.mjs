// Usage log through the real binary: POST /usage/events (ai.nvim's side), GET
// /usage with its filters and totals, the price table in /settings, and the
// events the server records for its own /ask requests.
import assert from "node:assert/strict";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { after, before, describe, it } from "node:test";

import { startServer } from "./helpers/server.mjs";

const SECRET_TEXT = "the user's secret prompt text";

function readLog(dir) {
  const file = path.join(dir, "usage.jsonl");
  if (!existsSync(file)) return [];
  return readFileSync(file, "utf8")
    .split("\n")
    .filter((l) => l.length > 0)
    .map((l) => JSON.parse(l));
}

// An OpenAI-compatible endpoint that answers with fixed token counts, so the
// recorded event of a successful /ask can be checked.
function startMock() {
  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(
        JSON.stringify({
          choices: [{ message: { content: "ok" }, finish_reason: "stop" }],
          usage: { prompt_tokens: 7, completion_tokens: 3 },
        }),
      );
    });
  });
  return new Promise((resolve) =>
    server.listen(0, "127.0.0.1", () => resolve({ port: server.address().port, close: () => new Promise((r) => server.close(r)) })),
  );
}

describe("/usage/events and /usage", () => {
  let dir;
  let server;
  before(async () => {
    dir = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-usage-"));
    server = await startServer({ configDir: dir });
  });
  after(async () => {
    await server.stop();
    rmSync(dir, { recursive: true, force: true });
  });

  it("starts empty", async () => {
    const res = await server.get("/usage");
    assert.equal(res.status, 200, res.text);
    assert.deepEqual(res.json.events, []);
    assert.equal(res.json.totals.requests, 0);
    assert.equal(res.json.totals.cost_usd, 0);
    assert.deepEqual(res.json.by_provider, {});
    assert.deepEqual(res.json.by_model, {});
  });

  it("records a valid event with the server's timestamp and the default source", async () => {
    const res = await server.post("/usage/events", {
      provider: "anthropic",
      model: "claude-haiku-4-5",
      stream: true,
      ok: true,
      prompt_tokens: 1000000,
      completion_tokens: 200000,
      duration_ms: 1234,
    });
    assert.equal(res.status, 200, res.text);
    assert.deepEqual(res.json, { ok: true });

    const lines = readLog(dir);
    assert.equal(lines.length, 1);
    assert.equal(lines[0].source, "ai.nvim");
    assert.match(lines[0].ts, /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$/);
    assert.equal(lines[0].provider, "anthropic");
    assert.equal(lines[0].stream, true);
    assert.equal(lines[0].prompt_tokens, 1000000);
    assert.deepEqual(Object.keys(lines[0]).sort(), ["completion_tokens", "duration_ms", "model", "ok", "prompt_tokens", "provider", "source", "stream", "ts"]);
  });

  it("keeps a given source (bounded) and optional error_kind", async () => {
    const res = await server.post("/usage/events", { provider: "openai", model: "gpt-4o", source: "other-editor", ok: false, error_kind: "timeout" });
    assert.equal(res.status, 200, res.text);
    const last = readLog(dir).at(-1);
    assert.equal(last.source, "other-editor");
    assert.equal(last.error_kind, "timeout");
    assert.equal((await server.post("/usage/events", { provider: "openai", model: "gpt-4o", source: "s".repeat(33) })).status, 400);
  });

  it("refuses an event that carries conversation text, naming the field, and stores nothing", async () => {
    const before = readLog(dir).length;
    for (const field of ["prompt", "text", "system", "messages", "input", "output", "content", "response"]) {
      const res = await server.post("/usage/events", { provider: "openai", model: "gpt-4o", [field]: SECRET_TEXT });
      assert.equal(res.status, 400, field);
      assert.match(res.json.error.message, new RegExp(`'${field}'`));
    }
    assert.equal(readLog(dir).length, before);
    assert.ok(!readFileSync(path.join(dir, "usage.jsonl"), "utf8").includes(SECRET_TEXT));
    assert.ok(!server.output().includes(SECRET_TEXT));
  });

  it("rejects invalid JSON, missing provider/model, negative numbers and oversized bodies", async () => {
    const before = readLog(dir).length;
    assert.equal((await server.post("/usage/events", "not json")).status, 400);
    assert.equal((await server.post("/usage/events", [])).status, 400);
    assert.equal((await server.post("/usage/events", { model: "gpt-4o" })).status, 400);
    assert.equal((await server.post("/usage/events", { provider: "openai" })).status, 400);
    assert.equal((await server.post("/usage/events", { provider: "openai", model: "gpt-4o", prompt_tokens: -1 })).status, 400);
    assert.equal((await server.post("/usage/events", { provider: "openai", model: "gpt-4o", duration_ms: -5 })).status, 400);
    assert.equal((await server.post("/usage/events", { provider: "openai", model: "gpt-4o", ok: "yes" })).status, 400);
    assert.equal((await server.post("/usage/events", { provider: "openai", model: "gpt-4o", note: "x".repeat(5000) })).status, 413);
    assert.equal((await server.post("/usage/events", { provider: "openai", model: "gpt-4o" }, { Origin: "http://evil.example" })).status, 403);
    assert.equal(readLog(dir).length, before);
  });

  it("aggregates totals, by provider and by model with the default prices", async () => {
    // So far: anthropic/claude-haiku-4-5 ok (1M in, 200k out: 1.0 + 1.0 USD), openai/gpt-4o failed (0 tokens).
    assert.equal((await server.post("/usage/events", { provider: "ollama", model: "llama3.1:8b", prompt_tokens: 50, completion_tokens: 60 })).status, 200);
    const res = await server.get("/usage");
    assert.equal(res.status, 200);
    assert.equal(res.json.events.length, 3);
    assert.equal(res.json.events[0].model, "llama3.1:8b", "newest first");
    assert.equal(res.json.totals.requests, 3);
    assert.equal(res.json.totals.ok, 2);
    assert.equal(res.json.totals.failed, 1);
    assert.equal(res.json.totals.prompt_tokens, 1000050);
    assert.equal(res.json.totals.completion_tokens, 200060);
    assert.equal(res.json.totals.unpriced, 0);
    assert.ok(Math.abs(res.json.totals.cost_usd - 2.0) < 1e-9, String(res.json.totals.cost_usd));
    assert.deepEqual(Object.keys(res.json.by_provider).sort(), ["anthropic", "ollama", "openai"]);
    assert.equal(res.json.by_provider.ollama.cost_usd, 0, "a local model is free");
    assert.ok(Math.abs(res.json.by_provider.anthropic.cost_usd - 2.0) < 1e-9);
    assert.equal(res.json.by_model["claude-haiku-4-5"].requests, 1);
  });

  it("filters by provider and by an inclusive since/until range, and caps the limit", async () => {
    const byProvider = await server.get("/usage?provider=openai");
    assert.equal(byProvider.json.totals.requests, 1);
    assert.deepEqual(Object.keys(byProvider.json.by_provider), ["openai"]);

    const [oldest] = readLog(dir);
    const future = "2999-01-01T00:00:00Z";
    assert.equal((await server.get(`/usage?since=${encodeURIComponent(oldest.ts)}&until=${future}`)).json.totals.requests, 3, "inclusive since");
    assert.equal((await server.get(`/usage?since=${future}`)).json.totals.requests, 0);
    assert.equal((await server.get("/usage?until=2000-01-01T00:00:00Z")).json.totals.requests, 0);
    assert.equal((await server.get("/usage?since=2000-01-01T00:00:00Z&until=" + future)).json.totals.requests, 3);

    const limited = await server.get("/usage?limit=1");
    assert.equal(limited.json.events.length, 1);
    assert.equal(limited.json.totals.requests, 3, "totals cover everything, not just the listed events");
    assert.equal((await server.get("/usage?limit=0")).status, 400);
    assert.equal((await server.get("/usage?limit=abc")).status, 400);
    assert.equal((await server.get("/usage?limit=999999")).status, 200, "a too-large limit is capped, not refused");
  });

  it("refuses since/until in any spelling but the stored one, naming the parameter", async () => {
    // Lexical comparison: a date-only or fractional until would silently exclude the whole day/second.
    for (const bad of ["2026-10-30", "2026-10-30T20:24:20.000Z", "2026-10-30T20:24:20+00:00", "now", ""]) {
      const res = await server.get(`/usage?until=${encodeURIComponent(bad)}`);
      assert.equal(res.status, 400, `until=${bad}: ${res.text}`);
      assert.match(res.json.error.message, /^until:/);
    }
    const since = await server.get("/usage?since=2026-10-30");
    assert.equal(since.status, 400);
    assert.match(since.json.error.message, /^since:/);
    assert.equal((await server.get("/usage?since=2000-01-01T00:00:00Z&until=2999-12-31T23:59:59Z")).status, 200);
  });

  it("nulls the cost as soon as an event has no price, and the settings' price table fixes that", async () => {
    assert.equal((await server.post("/usage/events", { provider: "openai", model: "gpt-99-turbo", prompt_tokens: 1000000, completion_tokens: 0 })).status, 200);
    let res = await server.get("/usage");
    assert.equal(res.json.totals.cost_usd, null);
    assert.equal(res.json.totals.unpriced, 1);
    assert.equal(res.json.by_provider.openai.cost_usd, null);
    assert.ok(Math.abs(res.json.by_provider.anthropic.cost_usd - 2.0) < 1e-9, "other groups keep their sums");

    const settings = await server.get("/settings");
    assert.equal(settings.status, 200);
    assert.ok(settings.json.prices["gpt-4o-mini"], "defaults are reported");
    const put = await server.put("/settings", { prices: { "gpt-99": { input_per_mtok: 3, output_per_mtok: 9 } } });
    assert.equal(put.status, 200, put.text);
    assert.deepEqual(put.json.prices["gpt-99"], { input_per_mtok: 3, output_per_mtok: 9 });
    assert.equal(JSON.parse(readFileSync(path.join(dir, "config.json"), "utf8")).settings.prices["gpt-99"].input_per_mtok, 3);

    res = await server.get("/usage");
    assert.equal(res.json.totals.unpriced, 0);
    assert.ok(Math.abs(res.json.totals.cost_usd - 5.0) < 1e-9, "longest prefix: gpt-99 prices gpt-99-turbo");
    assert.equal((await server.put("/settings", { prices: { "gpt-99": { input_per_mtok: -1, output_per_mtok: 9 } } })).status, 400);
    assert.equal((await server.put("/settings", { prices: "cheap" })).status, 400);
  });

  it("survives a torn line in the log", async () => {
    const file = path.join(dir, "usage.jsonl");
    const before = (await server.get("/usage")).json.totals.requests;
    const { appendFileSync } = await import("node:fs");
    appendFileSync(file, "{ torn\n");
    const res = await server.get("/usage");
    assert.equal(res.status, 200);
    assert.equal(res.json.totals.requests, before);
  });

  it("survives hand-edited lines whose fields have the wrong type", async () => {
    const file = path.join(dir, "usage.jsonl");
    const before = (await server.get("/usage")).json.totals.requests;
    const { appendFileSync } = await import("node:fs");
    appendFileSync(file, JSON.stringify({ ts: "2026-01-01T00:00:00Z", provider: "p", model: "m", source: null }) + "\n");
    appendFileSync(file, JSON.stringify({ ts: "2026-01-01T00:00:01Z", provider: "p", model: "m", error_kind: 7 }) + "\n");
    const res = await server.get("/usage");
    assert.equal(res.status, 200, res.text);
    assert.equal(res.json.totals.requests, before + 2, "the lines count, their odd fields read as empty");
    assert.equal(res.json.by_provider.p.requests, 2);
  });
});

describe("a hand-edited price table does not break /usage", () => {
  let dir;
  let server;
  before(async () => {
    dir = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-usage-prices-"));
    const { writeFileSync } = await import("node:fs");
    writeFileSync(
      path.join(dir, "config.json"),
      JSON.stringify({ version: 1, settings: { prices: { "gpt-4o": { input_per_mtok: "x", output_per_mtok: 10 } } } }),
    );
    server = await startServer({ configDir: dir });
  });
  after(async () => {
    await server.stop();
    rmSync(dir, { recursive: true, force: true });
  });

  it("drops the broken row from GET /settings and prices the event with the default", async () => {
    const settings = await server.get("/settings");
    assert.equal(settings.status, 200);
    assert.equal(settings.json.prices["gpt-4o"].input_per_mtok, 2.5, "the default, not the string");
    assert.equal((await server.post("/usage/events", { provider: "openai", model: "gpt-4o", prompt_tokens: 1000000 })).status, 200);
    const res = await server.get("/usage");
    assert.equal(res.status, 200, res.text);
    assert.ok(Math.abs(res.json.totals.cost_usd - 2.5) < 1e-9, String(res.json.totals.cost_usd));
  });
});

describe("the server records its own /ask requests", () => {
  let dir;
  let server;
  let mock;
  before(async () => {
    mock = await startMock();
    dir = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-usage-ask-"));
    server = await startServer({ configDir: dir });
    const add = await server.post("/models/custom", { id: "mocked", provider: "openai-compatible", base_url: `http://127.0.0.1:${mock.port}/v1` });
    assert.equal(add.status, 201, add.text);
  });
  after(async () => {
    await server.stop();
    await mock.close();
    rmSync(dir, { recursive: true, force: true });
  });

  it("logs ok=false with the backend's name for an unreachable backend, without any text", async () => {
    const ask = await server.post("/ask", { prompt: SECRET_TEXT, system: "be " + SECRET_TEXT, model: "llama3.1:8b" });
    assert.equal(ask.status, 502);
    const lines = readLog(dir);
    assert.equal(lines.length, 1);
    const ev = lines[0];
    assert.equal(ev.source, "ai-desktop");
    assert.equal(ev.provider, "ollama");
    assert.equal(ev.model, "llama3.1:8b");
    assert.equal(ev.ok, false);
    assert.equal(ev.stream, false);
    assert.equal(ev.error_kind, "backend_error");
    assert.ok(Number.isInteger(ev.duration_ms) && ev.duration_ms >= 0);
    assert.ok(!readFileSync(path.join(dir, "usage.jsonl"), "utf8").includes(SECRET_TEXT), "metadata only");

    const usage = await server.get("/usage");
    assert.equal(usage.json.totals.failed, 1);
    assert.equal(usage.json.by_provider.ollama.requests, 1);
  });

  it("logs no_key for a cloud model without a key and the prefix-chosen provider", async () => {
    assert.equal((await server.post("/ask", { prompt: "hi", model: "gpt-4o" })).status, 502);
    assert.equal((await server.post("/ask/stream", { prompt: "hi", model: "claude-haiku-4-5" })).status, 200);
    const lines = readLog(dir);
    assert.equal(lines.length, 3);
    assert.equal(lines[1].provider, "openai");
    assert.equal(lines[1].error_kind, "no_key");
    assert.equal(lines[2].provider, "anthropic");
    assert.equal(lines[2].stream, true);
    assert.equal(lines[2].ok, false, "a streamed error is still a failed request");
    assert.equal(lines[2].error_kind, "no_key");
  });

  it("logs an invalid request as such", async () => {
    assert.equal((await server.post("/ask", { model: "gpt-4o" })).status, 400);
    const last = readLog(dir).at(-1);
    assert.equal(last.ok, false);
    assert.equal(last.error_kind, "invalid_request");
    assert.equal(last.model, "gpt-4o");
  });

  it("still answers 400 (not 500) and logs the request when 'model' is not a string", async () => {
    const before = readLog(dir).length;
    const ask = await server.post("/ask", { model: 5 });
    assert.equal(ask.status, 400, ask.text);
    assert.match(ask.json.error.message, /'prompt'/);
    const stream = await server.post("/ask/stream", { model: [1] });
    assert.equal(stream.status, 400, stream.text);
    const lines = readLog(dir);
    assert.equal(lines.length, before + 2);
    assert.equal(lines[before].error_kind, "invalid_request");
    assert.equal(lines[before].model, "");
    assert.equal(lines[before + 1].stream, true);
  });

  it("bounds the model name it records like POST /usage/events does", async () => {
    const before = readLog(dir).length;
    assert.equal((await server.post("/ask", { model: "x".repeat(5000) })).status, 400);
    assert.equal((await server.post("/ask", { model: "a\u0001b" })).status, 400);
    const lines = readLog(dir);
    assert.equal(lines.length, before + 2);
    assert.equal(lines[before].model.length, 200, "cut to the store's bound");
    assert.equal(lines[before + 1].model, "(invalid)");
    const usage = await server.get("/usage");
    assert.equal(usage.status, 200);
    assert.ok(Object.keys(usage.json.by_model).every((m) => m.length <= 200));
  });

  it("logs ok=true with the token counts of a successful answer", async () => {
    const ask = await server.post("/ask", { prompt: "hi", model: "mocked" });
    assert.equal(ask.status, 200, ask.text);
    const last = readLog(dir).at(-1);
    assert.equal(last.ok, true);
    assert.equal(last.provider, "openai-compatible");
    assert.equal(last.model, "mocked");
    assert.equal(last.prompt_tokens, 7);
    assert.equal(last.completion_tokens, 3);
    assert.equal(last.error_kind, undefined);
    const usage = await server.get("/usage?provider=openai-compatible");
    assert.equal(usage.json.totals.ok, 1);
    assert.equal(usage.json.totals.prompt_tokens, 7);
    assert.equal(usage.json.totals.cost_usd, null, "a custom model has no default price");
  });
});
