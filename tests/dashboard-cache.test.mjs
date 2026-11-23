// The dashboard files carry a validator and may be kept but must be asked about: ETag, Cache-Control: no-cache and
// 304 for If-None-Match (cpp-httplib sends none of them for a mount point on its own).
import assert from "node:assert/strict";
import { after, before, describe, it } from "node:test";

import { startServer } from "./helpers/server.mjs";

describe("dashboard cache validators", () => {
  let server;
  before(async () => {
    server = await startServer();
  });
  after(async () => {
    await server.stop();
  });

  it("sends an ETag and Cache-Control: no-cache with every dashboard file, still with the CSP", async () => {
    for (const name of ["/", "/index.html", "/app.js", "/style.css", "/i18n.js", "/palette.css"]) {
      const res = await server.get(name);
      assert.equal(res.status, 200, name);
      assert.match(res.headers.etag, /^W\/"[0-9a-z-]+"$/, name);
      assert.equal(res.headers["cache-control"], "no-cache", name);
      assert.match(res.headers["content-security-policy"], /default-src 'none'/, name);
    }
  });

  it("answers 304 without a body to the ETag it sent, 200 to another one", async () => {
    const first = await server.get("/app.js");
    const again = await server.get("/app.js", { "If-None-Match": first.headers.etag });
    assert.equal(again.status, 304);
    assert.equal(again.text, "");
    assert.equal(again.headers.etag, first.headers.etag);
    assert.match(again.headers["content-security-policy"], /default-src 'none'/);
    const other = await server.get("/app.js", { "If-None-Match": 'W/"0-0"' });
    assert.equal(other.status, 200);
    assert.equal(other.text, first.text);
  });

  it("keeps the validators of one file for that file only", async () => {
    const a = await server.get("/app.js");
    const b = await server.get("/style.css");
    assert.notEqual(a.headers.etag, b.headers.etag);
    const wrong = await server.get("/style.css", { "If-None-Match": a.headers.etag });
    assert.equal(wrong.status, 200);
  });

  it("does not touch the routes: no ETag on the API, a missing file stays 404", async () => {
    const health = await server.get("/health");
    assert.equal(health.status, 200);
    assert.equal(health.headers.etag, undefined);
    const missing = await server.get("/no-such-file.js");
    assert.equal(missing.status, 404);
    assert.equal(missing.headers.etag, undefined);
    const outside = await server.get("/../CMakeLists.txt");
    assert.notEqual(outside.status, 304);
    assert.equal(outside.headers.etag, undefined);
  });
});
