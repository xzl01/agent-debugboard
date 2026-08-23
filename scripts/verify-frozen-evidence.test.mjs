import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { mkdtemp, mkdir, readFile, rename, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";
import { manifestBase, parseManifest } from "./verify-frozen-evidence.mjs";

const script = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "verify-frozen-evidence.mjs");

function sha256(text) {
  return createHash("sha256").update(text).digest("hex");
}

function run(root, map, stage, phase, extra = []) {
  try {
    const stdout = execFileSync("node", [script, "--root", root, "--map", map, "--stage", stage, "--phase", phase, ...extra], { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] });
    return { code: 0, stdout, stderr: "" };
  } catch (error) {
    return { code: error.status, stdout: error.stdout ?? "", stderr: error.stderr ?? "" };
  }
}

const MINI_MD = "# 2026-01-01 Mini HIL\n\nPASS\n";
const PACK_JSON = "{\"ok\":true}\n";
const EXTRA_TXT = "historical companion\n";

const MAP = [
  "source\ttarget\tfrozen\tcontent-action",
  "doc/testing/results/mini-hil.md\tdocs/testing/results/mini-hil.md\ttrue\tmove-bytes-only",
  "doc/testing/results/mini-hil.SHA256SUMS\tdocs/testing/results/mini-hil.SHA256SUMS\ttrue\tmove-bytes-only",
  "doc/testing/results/extra.txt\tdocs/testing/results/extra.txt\ttrue\tmove-bytes-only",
  "doc/testing/results/pack/pack-a.json\tdocs/testing/results/pack/pack-a.json\ttrue\tmove-bytes-only",
  "doc/testing/results/pack/SHA256SUMS\tdocs/testing/results/pack/SHA256SUMS\ttrue\tmove-bytes-only",
  "",
].join("\n");

async function makeFixture() {
  const root = await mkdtemp(path.join(os.tmpdir(), "frozen-evidence-"));
  const stage = path.join(root, "stage");
  const results = path.join(root, "doc/testing/results");
  await mkdir(path.join(results, "pack"), { recursive: true });
  await writeFile(path.join(results, "mini-hil.md"), MINI_MD);
  await writeFile(path.join(results, "extra.txt"), EXTRA_TXT);
  await writeFile(path.join(results, "mini-hil.SHA256SUMS"), `${sha256(MINI_MD)}  mini-hil.md\n${sha256(EXTRA_TXT)}  extra.txt\n`);
  await writeFile(path.join(results, "pack/pack-a.json"), PACK_JSON);
  await writeFile(path.join(results, "pack/SHA256SUMS"), `${sha256(PACK_JSON)}  doc/testing/results/pack/pack-a.json\n${sha256(MINI_MD)}  doc/testing/results/mini-hil.md\n`);
  await writeFile(path.join(root, "map.tsv"), MAP);
  return { root, stage, map: "map.tsv" };
}

async function applyMoves(root) {
  for (const line of MAP.split("\n").slice(1)) {
    if (!line.trim()) continue;
    const [source, target] = line.split("\t");
    await mkdir(path.dirname(path.join(root, target)), { recursive: true });
    await rename(path.join(root, source), path.join(root, target));
  }
}

test("manifestBase classifies basename and repository-root manifests", () => {
  assert.equal(manifestBase(parseManifest(`${sha256("a")}  a.md\n`), "doc/testing/results/x.SHA256SUMS"), "doc/testing/results");
  assert.equal(manifestBase(parseManifest(`${sha256("a")}  doc/testing/results/a.md\n`), "doc/testing/results/pack/SHA256SUMS"), "");
});

test("frozen evidence phases pass across a byte-for-byte move", async () => {
  const { root, stage, map } = await makeFixture();
  try {
    const pre = run(root, map, stage, "pre");
    assert.equal(pre.code, 0, pre.stderr);
    const preReceipt = JSON.parse(await readFile(path.join(stage, "frozen-pre-move.json"), "utf8"));
    assert.equal(preReceipt.ok, true);
    assert.equal(preReceipt.rows.length, 5);
    assert.equal(preReceipt.manifests.length, 2);
    assert.equal(preReceipt.manifests.every((manifest) => manifest.failed.length === 0), true);

    await applyMoves(root);

    const post = run(root, map, stage, "post");
    assert.equal(post.code, 0, post.stderr);
    const postReceipt = JSON.parse(await readFile(path.join(stage, "frozen-post-content.json"), "utf8"));
    assert.equal(postReceipt.ok, true);
    assert.equal(postReceipt.rows.every((row) => row.preMoveMatch === true), true);
    const rootManifest = postReceipt.manifests.find((manifest) => manifest.base === "<repository-root>");
    assert.equal(rootManifest.failed.length, 2);
    assert.equal(rootManifest.failed.every((failure) => failure.reason === "missing"), true);

    const expected = run(root, map, stage, "expected-old-path-failures");
    assert.equal(expected.code, 0, expected.stderr);
    const expectedReceipt = JSON.parse(await readFile(path.join(stage, "frozen-expected-old-path-failures.json"), "utf8"));
    assert.equal(expectedReceipt.ok, true);
    assert.equal(expectedReceipt.allowlist.length, 2);
    assert.equal(expectedReceipt.newFailures.length, 2);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test("post phase rejects corrupted moved content", async () => {
  const { root, stage, map } = await makeFixture();
  try {
    assert.equal(run(root, map, stage, "pre").code, 0);
    await applyMoves(root);
    await writeFile(path.join(root, "docs/testing/results/mini-hil.md"), "# tampered\n");
    const post = run(root, map, stage, "post");
    assert.equal(post.code, 65);
    const postReceipt = JSON.parse(await readFile(path.join(stage, "frozen-post-content.json"), "utf8"));
    assert.equal(postReceipt.ok, false);
    assert.equal(postReceipt.rows.find((row) => row.target === "docs/testing/results/mini-hil.md").preMoveMatch, false);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test("expected-old-path-failures rejects an old path that still resolves", async () => {
  const { root, stage, map } = await makeFixture();
  try {
    assert.equal(run(root, map, stage, "pre").code, 0);
    await applyMoves(root);
    await writeFile(path.join(root, "docs/testing/results/mini-hil.md"), MINI_MD);
    await mkdir(path.join(root, "doc/testing/results/pack"), { recursive: true });
    await writeFile(path.join(root, "doc/testing/results/pack/pack-a.json"), PACK_JSON);
    const post = run(root, map, stage, "post");
    assert.equal(post.code, 0, post.stderr);
    const expected = run(root, map, stage, "expected-old-path-failures");
    assert.equal(expected.code, 65);
    const receipt = JSON.parse(await readFile(path.join(stage, "frozen-expected-old-path-failures.json"), "utf8"));
    assert.equal(receipt.ok, false);
    assert.equal(receipt.checks.some((check) => check.check.includes("pack-a.json") && check.ok === false), true);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});
