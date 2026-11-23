import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import { test } from "node:test";
import { repoRoot } from "./helpers/server.mjs";

// CLAUDE.md's "Known findings" lists what is wrong in dashboard/. A finding that names code must still be there: one the
// source has fixed is removed from the list, one the list names must exist in the source (it went stale once).
const read = (name) => readFileSync(path.join(repoRoot, name), "utf8");
const section = () => {
  const text = read("CLAUDE.md");
  const start = text.indexOf("- Known findings");
  assert.ok(start >= 0, "CLAUDE.md has a Known findings entry");
  const end = text.indexOf("\n- ", start + 1);
  return text.slice(start, end < 0 ? undefined : end);
};

test("the known findings do not name a rule or a handler that the dashboard has already fixed or never had", () => {
  const known = section();
  const css = read("dashboard/style.css");
  const js = read("dashboard/app.js");
  const open = (text) => /none open/i.test(known) === false && known.includes(text);
  // a finding about ::-webkit-scrollbar is open only while style.css still has it
  assert.equal(open("::-webkit-scrollbar"), css.includes("::-webkit-scrollbar"));
  // a finding about the Enter handling is open only while the script has an Enter handler to be made IME-safe
  assert.equal(open("isComposing"), /key\s*===?\s*['"]Enter['"]/.test(js));
});

test("the dashboard has no ::-webkit-scrollbar rule and no Enter handler (what CLAUDE.md says)", () => {
  assert.ok(!read("dashboard/style.css").includes("::-webkit-scrollbar"));
  assert.ok(!/key\s*===?\s*['"]Enter['"]/.test(read("dashboard/app.js")));
  assert.match(section(), /none open/i);
});
