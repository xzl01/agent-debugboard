import assert from "node:assert/strict";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { discoverTests } from "./run-tests.mjs";

test("discovers and classifies every Web test file", async (t) => {
  const root = await mkdtemp(path.join(os.tmpdir(), "linkr-web-tests-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  await mkdir(path.join(root, "src", "nested"), { recursive: true });
  await mkdir(path.join(root, "scripts"), { recursive: true });
  await writeFile(path.join(root, "src", "node.test.ts"), 'import test from "node:test";\n');
  await writeFile(path.join(root, "src", "nested", "browser.test.tsx"), 'import { it } from "vitest";\n');
  await writeFile(path.join(root, "src", "nested", "multiline.test.tsx"), 'import {\n  it,\n} from "vitest";\n');
  await writeFile(path.join(root, "scripts", "tool.test.mjs"), 'import test from "node:test";\n');
  await writeFile(path.join(root, "scripts", "common.test.cjs"), 'const test = require("node:test");\n');
  await writeFile(path.join(root, "src", "ignored.ts"), "export {};\n");

  assert.deepEqual(await discoverTests(root), {
    node: ["scripts/common.test.cjs", "scripts/tool.test.mjs", "src/node.test.ts"],
    browser: ["src/nested/browser.test.tsx", "src/nested/multiline.test.tsx"],
    unclassified: [],
  });
});

test("reports test files that do not declare their runner", async (t) => {
  const root = await mkdtemp(path.join(os.tmpdir(), "linkr-web-tests-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  await mkdir(path.join(root, "src"), { recursive: true });
  await writeFile(path.join(root, "src", "orphan.test.ts"), "export {};\n");

  assert.deepEqual((await discoverTests(root)).unclassified, ["src/orphan.test.ts"]);
});

test("ignores runner-like imports inside comments and template fixtures", async (t) => {
  const root = await mkdtemp(path.join(os.tmpdir(), "linkr-web-tests-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  await mkdir(path.join(root, "src"), { recursive: true });
  await writeFile(path.join(root, "src", "node.test.mjs"), [
    'import test from "node:test";',
    "/* import { it } from \"vitest\"; */",
    "const fixture = `",
    'import { it } from "vitest";',
    "`;",
  ].join("\n"));
  await writeFile(path.join(root, "src", "orphan.test.mjs"), [
    "const fixture = `",
    'import test from "node:test";',
    "`;",
  ].join("\n"));

  assert.deepEqual(await discoverTests(root), {
    node: ["src/node.test.mjs"],
    browser: [],
    unclassified: ["src/orphan.test.mjs"],
  });
});
