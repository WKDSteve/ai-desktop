// POST /decision takes the id the dashboard got with the decision point (a number) and never answers 500 for a
// body of the wrong shape.
import assert from "node:assert/strict";
import { after, before, describe, it } from "node:test";

import { startServer } from "./helpers/server.mjs";

// The server prints an event as it pushes it; the pipe to this process may be a moment behind the answer.
async function printed(server, pattern) {
  const deadline = Date.now() + 3000;
  while (!pattern.test(server.output()) && Date.now() < deadline) await new Promise((r) => setTimeout(r, 50));
  assert.match(server.output(), pattern);
}

describe("POST /decision", () => {
  let server;
  before(async () => {
    server = await startServer();
  });
  after(async () => {
    await server.stop();
  });

  it("accepts a numeric id and tells the answer on the event stream and stdout", async () => {
    const res = await server.post("/decision", { id: 1795272104, answer: "allow" });
    assert.equal(res.status, 200, res.text);
    await printed(server, /\[human\] Decision \[1795272104\] -> allow/);
  });

  it("accepts a string id", async () => {
    const res = await server.post("/decision", { id: "abc", answer: "deny" });
    assert.equal(res.status, 200, res.text);
    await printed(server, /\[human\] Decision \[abc\] -> deny/);
  });

  it("does not fail on fields of another type or a body that is no object", async () => {
    assert.equal((await server.post("/decision", { id: { a: 1 }, answer: 5 })).status, 200);
    await printed(server, /\[human\] Decision \[unknown\] -> unknown/);
    assert.equal((await server.post("/decision", "[1,2]")).status, 400);
    assert.equal((await server.post("/decision", "not json")).status, 400);
  });
});
