// The install line of the READMEs is the one the `published` job of rolling.yml runs as it is typed, on a
// throw-away VM without a GitHub token: that job is the proof that the line installs, so a README that names
// another line would point people at something nobody checks.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import { test } from "node:test";
import { repoRoot } from "./helpers/server.mjs";

const read = (rel) => readFileSync(path.join(repoRoot, rel), "utf8");

/** The line of the `published` job, with the repository of the READMEs' links filled in. */
function publishedLine() {
  const workflow = read(".github/workflows/rolling.yml");
  const match = workflow.match(/^\s+LINE: (irm .+)$/m);
  assert.ok(match, "rolling.yml: the published job has no LINE");
  return match[1].trim().replace("${{ github.repository }}", "WKDSteve/ai-desktop");
}

/** The one-liners `irm ... | iex` a README names. */
function installLines(text) {
  return text
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => /^irm .*\|\s*iex$/.test(l));
}

for (const readme of ["README.md", "README.de.md"]) {
  test(`${readme} names exactly the install line the published job runs`, () => {
    const line = publishedLine();
    const lines = installLines(read(readme));
    assert.ok(lines.length > 0, `${readme} names no install line`);
    for (const l of lines) assert.equal(l, line, `${readme}: an install line the CI does not run`);
  });
}
