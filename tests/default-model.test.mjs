// The model of a request without "model": a setting or AI_DESKTOP_DEFAULT_MODEL wins; otherwise the built-in
// llama3.1:8b-q5_K_M if Ollama has it, else the installed model changed last; /health says whether it is installed.
import assert from "node:assert/strict";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { after, describe, it } from "node:test";

import { startServer } from "./helpers/server.mjs";

// A fake Ollama with the given models; remembers the "model" of every /api/generate.
function startFakeOllama(models) {
  const state = { generated: [] };
  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => {
      res.setHeader("Content-Type", "application/json");
      if (req.method === "GET" && req.url === "/api/tags") {
        res.end(JSON.stringify({ models: models.map(([name, modified_at]) => ({ name, modified_at })) }));
      } else if (req.method === "POST" && req.url === "/api/generate") {
        const { model } = JSON.parse(Buffer.concat(chunks).toString("utf8"));
        state.generated.push(model);
        res.end(JSON.stringify({ response: "ok", done: true }));
      } else {
        res.statusCode = 404;
        res.end("{}");
      }
    });
  });
  return new Promise((resolve) =>
    server.listen(0, "127.0.0.1", () =>
      resolve({ host: `http://127.0.0.1:${server.address().port}`, state, close: () => new Promise((r) => server.close(r)) }),
    ),
  );
}

const open = [];
after(async () => {
  for (const close of open.reverse()) await close();
});

// Starts a fake Ollama and the server on it; `settings` go into config.json, `env` into the server's environment.
async function setup(models, { settings = {}, env = {} } = {}) {
  const ollama = await startFakeOllama(models);
  const configDir = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-defmodel-"));
  writeFileSync(path.join(configDir, "config.json"), JSON.stringify({ version: 1, settings }));
  const server = await startServer({
    configDir,
    env: { AI_DESKTOP_OLLAMA_HOST: ollama.host, AI_DESKTOP_DEFAULT_MODEL: "", ...env },
  });
  open.push(async () => {
    await server.stop();
    await ollama.close();
    rmSync(configDir, { recursive: true, force: true });
  });
  return { server, ollama };
}

async function health(server) {
  const res = await server.get("/health");
  assert.equal(res.status, 200);
  return res.json.default_model;
}

describe("default model from the installed Ollama models", () => {
  it("takes the installed model changed last when the built-in one is missing", async () => {
    const { server, ollama } = await setup([
      ["mistral:7b", "2025-09-01T10:00:00Z"],
      ["qwen3:8b", "2026-03-02T09:00:00Z"],
    ]);
    assert.deepEqual(await health(server), { name: "qwen3:8b", source: "installed", installed: true });
    const res = await server.post("/ask", { prompt: "hi" });
    assert.equal(res.status, 200, res.text);
    assert.deepEqual(ollama.state.generated, ["qwen3:8b"]);
  });

  it("keeps the built-in model when Ollama has it", async () => {
    const { server } = await setup([
      ["llama3.1:8b-q5_K_M", "2024-01-01T00:00:00Z"],
      ["qwen3:8b", "2026-03-02T09:00:00Z"],
    ]);
    assert.deepEqual(await health(server), { name: "llama3.1:8b-q5_K_M", source: "builtin", installed: true });
  });

  it("never replaces a model that was chosen, and says when it is not installed", async () => {
    const { server, ollama } = await setup([["qwen3:8b", "2026-03-02T09:00:00Z"]], { settings: { default_model: "gone:1b" } });
    assert.deepEqual(await health(server), { name: "gone:1b", source: "config", installed: false });
    await server.post("/ask", { prompt: "hi" });
    assert.deepEqual(ollama.state.generated, ["gone:1b"]);
  });

  it("lets AI_DESKTOP_DEFAULT_MODEL win over the setting", async () => {
    const { server } = await setup([["qwen3:8b", "2026-03-02T09:00:00Z"]], {
      settings: { default_model: "gone:1b" },
      env: { AI_DESKTOP_DEFAULT_MODEL: "qwen3:8b" },
    });
    assert.deepEqual(await health(server), { name: "qwen3:8b", source: "env", installed: true });
  });

  it("reports the built-in model as not installed when Ollama has no model", async () => {
    const { server } = await setup([]);
    assert.deepEqual(await health(server), { name: "llama3.1:8b-q5_K_M", source: "builtin", installed: false });
  });

  it("reports installed as null when Ollama is not reachable", async () => {
    const server = await startServer({ env: { AI_DESKTOP_DEFAULT_MODEL: "" } });
    try {
      assert.deepEqual(await health(server), { name: "llama3.1:8b-q5_K_M", source: "builtin", installed: null });
    } finally {
      await server.stop();
    }
  });
});
