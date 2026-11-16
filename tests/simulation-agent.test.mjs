// The simulation agent (a placeholder until a real agent runs) speaks English, as the rest of the server's events
// do, and every agent it names has its card on the dashboard (dot-<agent> and msg-<agent> in index.html).
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import { test } from "node:test";
import { repoRoot, startServer } from "./helpers/server.mjs";

const GERMAN = /[äöüÄÖÜß]|\b(Projektstruktur|erkannt|Prüfe|verfügbar|bereit|Soll ich|jetzt|starten|Warte|Entscheidung|gestartet|analysiere)\b|\bPlaner\b/;

test("the simulation agent's first events are English and name agents the dashboard has a card for", async () => {
  const server = await startServer();
  try {
    // "[agent] message" lines on stdout; the first two come after about 1 s and 4 s.
    const events = () => server.output().split(/\r?\n/).filter((l) => /^\[[^\]]+\] /.test(l));
    const deadline = Date.now() + 10000;
    while (events().length < 2 && Date.now() < deadline) await new Promise((r) => setTimeout(r, 200));
    const lines = events();
    assert.ok(lines.length >= 2, `the simulation agent sent fewer than two events:\n${server.output()}`);
    const html = readFileSync(path.join(repoRoot, "dashboard", "index.html"), "utf8");
    for (const line of lines) {
      assert.doesNotMatch(line, GERMAN, "a German event of the simulation agent");
      const agent = line.slice(1, line.indexOf("]"));
      assert.ok(html.includes(`id="dot-${agent}"`), `no card on the dashboard for the agent ${agent}`);
      assert.ok(html.includes(`id="msg-${agent}"`), `no message line on the dashboard for the agent ${agent}`);
    }
  } finally {
    await server.stop();
  }
});
