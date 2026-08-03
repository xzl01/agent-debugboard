import assert from "node:assert/strict";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { checkTestRegistration, formatFailures } from "./check-test-registration.mjs";

async function fixture() {
  const root = await mkdtemp(path.join(os.tmpdir(), "linkr-test-registration-"));
  const tests = path.join(root, "apps", "radxa_linkr_debugger", "tests");
  await mkdir(path.join(tests, "model_host"), { recursive: true });
  await mkdir(path.join(root, "web", "src"), { recursive: true });
  await mkdir(path.join(root, "web", "scripts"), { recursive: true });
  await writeFile(path.join(tests, "model_host", "test_model.c"), "int main(void) { return 0; }\n");
  await writeFile(path.join(tests, "test_offline.py"), "import unittest\nclass T(unittest.TestCase): pass\nunittest.main()\n");
  await writeFile(path.join(tests, "test_hil.py"), "def test_live(): return True\n");
  await writeFile(path.join(tests, "run_unit_tests.sh"), "test_model.c\ntest_offline.py\n");
  await writeFile(path.join(root, "web", "src", "unit.test.ts"), 'import test from "node:test";\n');
  return root;
}

test("accepts a complete firmware and Web test inventory", async (t) => {
  const root = await fixture();
  t.after(() => rm(root, { recursive: true, force: true }));
  assert.deepEqual(await checkTestRegistration(root), { ok: true, failures: [] });
});

test("reports orphaned C, Python, and Web tests together", async (t) => {
  const root = await fixture();
  t.after(() => rm(root, { recursive: true, force: true }));
  const runner = path.join(root, "apps", "radxa_linkr_debugger", "tests", "run_unit_tests.sh");
  await writeFile(runner, "#!/bin/sh\n# test_model.c test_offline.py\n");
  await writeFile(path.join(root, "web", "src", "orphan.test.ts"), "export {};\n");

  const result = await checkTestRegistration(root);
  assert.equal(result.ok, false);
  assert.match(formatFailures(result.failures), /test_model\.c: not registered/);
  assert.match(formatFailures(result.failures), /test_offline\.py: offline unit test is not registered/);
  assert.match(formatFailures(result.failures), /web\/src\/orphan\.test\.ts: test runner is not declared/);
  assert.doesNotMatch(formatFailures(result.failures), /test_hil\.py/);
});
