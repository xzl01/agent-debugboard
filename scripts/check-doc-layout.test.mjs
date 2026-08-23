import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtemp, mkdir, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";
import { checkDocLayout, findLegacyReferences } from "./check-doc-layout.mjs";

const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

async function withFixture(files, callback) {
  const root = await mkdtemp(path.join(os.tmpdir(), "doc-layout-"));
  try {
    for (const [rel, content] of Object.entries(files)) {
      const abs = path.join(root, rel);
      await mkdir(path.dirname(abs), { recursive: true });
      await writeFile(abs, content);
    }
    await callback(root, Object.keys(files));
  } finally {
    await rm(root, { recursive: true, force: true });
  }
}

test("check-doc-layout passes a clean canonical tree", async () => {
  await withFixture({
    "README.md": "# Root\n\nSee [canonical](docs/reference/persistent-configuration.md) and `docs/testing/hil-functional-test-spec.md`.\n",
    "docs/reference/persistent-configuration.md": "# Persistent Configuration\n\nSee [HIL](../testing/hil-functional-test-spec.md).\n",
    "docs/testing/hil-functional-test-spec.md": "# HIL\n",
    "nix/package.nix": "{ }: {\n  postInstall = ''\n    install -Dm644 docs/reference/openocd/README.md \"$out/share/doc/radxa-linkr-debugger/openocd.md\"\n  '';\n}\n",
  }, async (root, files) => {
    const result = await checkDocLayout(root, files);
    assert.equal(result.ok, true, JSON.stringify(result.violations));
    assert.equal(result.violations.length, 0);
  });
});

test("check-doc-layout rejects a live legacy doc/ reference", async () => {
  await withFixture({
    "README.md": "# Root\n\nSee [canonical](doc/persistent-configuration.md).\n",
    "docs/reference/persistent-configuration.md": "# Persistent Configuration\n",
  }, async (root, files) => {
    const result = await checkDocLayout(root, files);
    assert.equal(result.ok, false);
    assert.equal(result.violations.length, 1);
    assert.equal(result.violations[0].path, "README.md");
    assert.equal(result.violations[0].line, 3);
  });
});

test("check-doc-layout CLI exits 65 and names the violating file", async () => {
  await withFixture({
    "README.md": "# Root\n\nSee [canonical](doc/persistent-configuration.md).\n",
  }, async (root) => {
    const list = path.join(root, "files.txt");
    await writeFile(list, "README.md\n");
    let code = 0;
    let stderr = "";
    try {
      execFileSync("node", [path.join(repositoryRoot, "scripts/check-doc-layout.mjs"), "--root", root, "--files", list], { encoding: "utf8" });
    } catch (error) {
      code = error.status;
      stderr = error.stderr;
    }
    assert.equal(code, 65);
    assert.match(stderr, /README\.md:3/);
  });
});

test("check-doc-layout allows frozen evidence and install destinations", async () => {
  await withFixture({
    "docs/testing/results/2026-07-30-example-hil.md": "# HIL\n\nArtifacts were archived to `doc/testing/results/`.\n",
    "docs/testing/reports/2026-07-17-example.md": "Historical path: doc/testing/hil-functional-test-spec.md\n",
    "package.nix": "''$out/share/doc/radxa-linkr-debugger/testing/hil-functional-test-spec.md''\n",
    "docs/user/cli.md": "Mentions jsdoc/typedoc and typedoc/ tooling plus `some-doc/` and my.doc/words.\n",
  }, async (root, files) => {
    const result = await checkDocLayout(root, files);
    assert.equal(result.ok, true, JSON.stringify(result.violations));
    assert.equal(result.skipped.frozen, 2);
  });
});

test("check-doc-layout rejects relative legacy forms", async () => {
  const content = [
    "[a](../../doc/openocd/README.md)",
    "[b](./doc/mcp-server.md)",
    "[c](../doc/adc-telemetry.md)",
  ].join("\n");
  const hits = findLegacyReferences(content);
  assert.equal(hits.length, 3);
  assert.deepEqual(hits.map((hit) => hit.line), [1, 2, 3]);
});

test("check-doc-layout skips tooling, binaries, and missing files", async () => {
  await withFixture({
    "scripts/check-doc-layout.mjs": "// mentions doc/ by design\n",
    "docs/assets/architecture/diagram.png": "fake-binary doc/inside\n",
    "web/dist/bundle.js": "doc/inside-build-output\n",
  }, async (root, files) => {
    const result = await checkDocLayout(root, [...files, "deleted/tracked-file.md"]);
    assert.equal(result.ok, true, JSON.stringify(result.violations));
    assert.equal(result.skipped.missing, 1);
    assert.equal(result.scanned, 0);
  });
});
