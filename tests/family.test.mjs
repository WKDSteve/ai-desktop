// The family: hub.json in the config folder says that this server runs (src/family.hpp, appkit's docs/FAMILY.md).
import assert from "node:assert/strict";
import { describe, it } from "node:test";

import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import os from "node:os";
import path from "node:path";

import { runToExit, startServer } from "./helpers/server.mjs";

describe("family discovery file", () => {
  it("names the port, the process and the instance /health says, in appkit's exact format", async () => {
    const server = await startServer();
    try {
      const text = readFileSync(path.join(server.configDir, "hub.json"), "utf8");
      const record = JSON.parse(text);
      // appkit reads it with deny_unknown_fields: these keys, this order, pretty-printed, a final newline.
      assert.deepEqual(Object.keys(record), ["app", "port", "pid", "version", "instance"]);
      assert.equal(text, JSON.stringify(record, null, 2) + "\n");
      const health = (await server.get("/health")).json;
      assert.equal(record.app, "ai-desktop");
      assert.equal(health.app, "ai-desktop");
      assert.equal(record.port, server.port);
      assert.equal(record.instance, health.instance);
      assert.equal(record.version, health.version);
      assert.match(record.instance, /^[0-9a-f]{32}$/);
      assert.ok(Number.isInteger(record.pid) && record.pid > 0);
      assert.ok(!/secret|token|key/i.test(text), "no secret in the file");
    } finally {
      await server.stop();
    }
  });

  it("is new with every start, and a start whose port is taken leaves the running server's file alone", async () => {
    const configDir = mkdtempSync(path.join(os.tmpdir(), "ai-desktop-family-"));
    try {
      const first = await startServer({ configDir });
      let firstInstance;
      try {
        firstInstance = JSON.parse(readFileSync(path.join(configDir, "hub.json"), "utf8")).instance;
        const second = await runToExit(["--port", String(first.port)], { AI_DESKTOP_CONFIG_DIR: configDir });
        assert.notEqual(second.code, 0);
        assert.equal(JSON.parse(readFileSync(path.join(configDir, "hub.json"), "utf8")).instance, firstInstance);
      } finally {
        await first.stop();
      }
      // A stop by signal (POSIX) takes the file away; on Windows the test can only kill the process, and the
      // reader of appkit does not believe a file whose process is gone.
      if (process.platform !== "win32") {
        assert.ok(!existsSync(path.join(configDir, "hub.json")), "hub.json removed on SIGTERM");
      }
      const again = await startServer({ configDir });
      try {
        const next = JSON.parse(readFileSync(path.join(configDir, "hub.json"), "utf8"));
        assert.notEqual(next.instance, firstInstance);
        assert.equal(next.port, again.port);
      } finally {
        await again.stop();
      }
    } finally {
      rmSync(configDir, { recursive: true, force: true });
    }
  });
});
