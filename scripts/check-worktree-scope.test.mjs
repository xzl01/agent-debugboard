import { createHash } from "node:crypto";
import assert from "node:assert/strict";
import { mkdtemp, mkdir, rm, writeFile } from "node:fs/promises";
import { spawnSync, execFileSync } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";
import { checkWorktreeScope, formatFindings, EXIT_MISMATCH, EXIT_USAGE, EXIT_UNKNOWN_ADDITION } from "./check-worktree-scope.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const SCRIPT = path.join(ROOT, "scripts", "check-worktree-scope.mjs");

function git(root, args) {
  execFileSync("git", args, { cwd: root, stdio: "ignore" });
}

function sha256Hex(text) {
  return createHash("sha256").update(text).digest("hex");
}

function manifestLine(content, relPath) {
  return `${sha256Hex(content)}  ${relPath}`;
}

async function createBaseline({ malformedManifest = false } = {}) {
  const baseline = await mkdtemp(path.join(os.tmpdir(), "worktree-scope-baseline-"));
  await writeFile(path.join(baseline, "planned-paths.txt"), "planned/keep.txt\n");
  await writeFile(path.join(baseline, "planned-paths.schema.tsv"), "schema_version\t1\ngenerated_exclusions\tweb/node_modules/**, web/dist/**, build/**, **/target/**, .git/**, .omo/**, .playwright-mcp/** remain non-planned unless explicitly listed\n");
  await writeFile(path.join(baseline, "universe.paths"), ".github/workflows/version-bump.yml\ncmd-ng/target/generated.bin\ntracked/delete-me.txt\n");
  await writeFile(path.join(baseline, "nonplanned.SHA256SUMS"), malformedManifest ? "not-a-hash  .github/workflows/version-bump.yml\n" : [
    manifestLine("baseline\n", ".github/workflows/version-bump.yml"),
    manifestLine("planned\n", "planned/keep.txt"),
    manifestLine("generated\n", "cmd-ng/target/generated.bin"),
    manifestLine("evidence\n", ".omo/evidence/trace.txt"),
  ].join("\n") + "\n");
  return baseline;
}

async function createRepo({ planned = false, evidence = false, playwright = false, generatedMissing = false, unknown = false, dirtyBaseline = false, deletedTracked = false } = {}) {
  const root = await mkdtemp(path.join(os.tmpdir(), "worktree-scope-root-"));
  await mkdir(path.join(root, ".github", "workflows"), { recursive: true });
  await writeFile(path.join(root, ".github", "workflows", "version-bump.yml"), "baseline\n");
  git(root, ["init"]);
  git(root, ["config", "user.name", "Test User"]);
  git(root, ["config", "user.email", "test@example.invalid"]);
  git(root, ["add", ".github/workflows/version-bump.yml"]);
  git(root, ["commit", "-m", "base"]);
  await mkdir(path.join(root, "cmd-ng", "target"), { recursive: true });
  if (!generatedMissing) await writeFile(path.join(root, "cmd-ng", "target", "generated.bin"), "generated\n");
  await mkdir(path.join(root, "tracked"), { recursive: true });
  await writeFile(path.join(root, "tracked", "delete-me.txt"), "tracked\n");
  git(root, ["add", "tracked/delete-me.txt"]);
  git(root, ["commit", "-m", "tracked baseline"]);
  if (deletedTracked) await rm(path.join(root, "tracked", "delete-me.txt"));
  if (dirtyBaseline) await writeFile(path.join(root, ".github", "workflows", "version-bump.yml"), "dirty\n");
  if (planned) {
    await mkdir(path.join(root, "planned"), { recursive: true });
    await writeFile(path.join(root, "planned", "keep.txt"), "planned\n");
  }
  if (evidence) {
    await mkdir(path.join(root, ".omo", "evidence"), { recursive: true });
    await writeFile(path.join(root, ".omo", "evidence", "trace.txt"), "evidence\n");
  }
  if (playwright) {
    await mkdir(path.join(root, ".playwright-mcp"), { recursive: true });
    await writeFile(path.join(root, ".playwright-mcp", "page-2026-08-21T11-19-08-109Z.yml"), "page\n");
  }
  if (unknown) {
    await mkdir(path.join(root, "scratch"), { recursive: true });
    await writeFile(path.join(root, "scratch", "rogue.txt"), "rogue\n");
  }
  return root;
}

test("rejects missing CLI arguments", async (t) => {
  const baseline = await createBaseline();
  t.after(async () => {
    await rm(baseline, { recursive: true, force: true });
  });

  const result = spawnSync(process.execPath, [SCRIPT], { encoding: "utf8" });
  assert.equal(result.status, EXIT_USAGE);
  assert.match(result.stderr, /usage: node scripts\/check-worktree-scope\.mjs/);
  assert.match(result.stderr, /--planned <planned-paths\.txt>/);
});

test("rejects an unknown new path", async (t) => {
  const baseline = await createBaseline();
  const root = await createRepo({ unknown: true });
  t.after(async () => {
    await rm(root, { recursive: true, force: true });
    await rm(baseline, { recursive: true, force: true });
  });

  const result = await checkWorktreeScope(root, baseline, path.join(baseline, "planned-paths.txt"));
  assert.equal(result.ok, false);
  assert.equal(result.exitCode, EXIT_UNKNOWN_ADDITION);
  assert.deepEqual(result.additions, ["scratch/rogue.txt"]);
  assert.equal(result.missing.length, 0);
  assert.match(formatFindings(result), /\[addition\] scratch\/rogue\.txt/);
});

test("allows an explicit planned path", async (t) => {
  const baseline = await createBaseline();
  const root = await createRepo({ planned: true });
  await writeFile(path.join(root, "planned", "keep.txt"), "planned-mutated\n");
  t.after(async () => {
    await rm(root, { recursive: true, force: true });
    await rm(baseline, { recursive: true, force: true });
  });

  const result = await checkWorktreeScope(root, baseline, path.join(baseline, "planned-paths.txt"));
  assert.equal(result.ok, true);
  assert.equal(result.exitCode, 0);
  assert.deepEqual(result.mismatches, []);
  assert.match(formatFindings(result), /worktree scope OK:/);
});

test("allows generated and evidence content changes under schema exclusions", async (t) => {
  const baseline = await createBaseline();
  const root = await createRepo({ evidence: true });
  await writeFile(path.join(root, "cmd-ng", "target", "generated.bin"), "generated-mutated\n");
  await writeFile(path.join(root, ".omo", "evidence", "trace.txt"), "evidence-mutated\n");
  t.after(async () => {
    await rm(root, { recursive: true, force: true });
    await rm(baseline, { recursive: true, force: true });
  });

  const result = await checkWorktreeScope(root, baseline, path.join(baseline, "planned-paths.txt"));
  assert.equal(result.ok, true);
  assert.equal(result.exitCode, 0);
  assert.deepEqual(result.mismatches, []);
});

test("allows generated QA artifacts under .playwright-mcp", async (t) => {
  const baseline = await createBaseline();
  const root = await createRepo({ playwright: true });
  t.after(async () => {
    await rm(root, { recursive: true, force: true });
    await rm(baseline, { recursive: true, force: true });
  });

  const result = await checkWorktreeScope(root, baseline, path.join(baseline, "planned-paths.txt"));
  assert.equal(result.ok, true);
  assert.equal(result.exitCode, 0);
  assert.deepEqual(result.additions, []);
});

test("allows generated missing paths under generated_exclusions", async (t) => {
  const baseline = await createBaseline();
  const root = await createRepo({ generatedMissing: true });
  t.after(async () => {
    await rm(root, { recursive: true, force: true });
    await rm(baseline, { recursive: true, force: true });
  });

  const result = await checkWorktreeScope(root, baseline, path.join(baseline, "planned-paths.txt"));
  assert.equal(result.ok, true);
  assert.equal(result.exitCode, 0);
  assert.deepEqual(result.missing, []);
});

test("rejects an unstaged deletion of a tracked baseline path", async (t) => {
  const baseline = await createBaseline();
  const root = await createRepo({ deletedTracked: true });
  t.after(async () => {
    await rm(root, { recursive: true, force: true });
    await rm(baseline, { recursive: true, force: true });
  });

  const result = await checkWorktreeScope(root, baseline, path.join(baseline, "planned-paths.txt"));
  assert.equal(result.ok, false);
  assert.equal(result.exitCode, 66);
  assert.deepEqual(result.missing, ["tracked/delete-me.txt"]);
});

test("rejects a baseline-known dirty path", async (t) => {
  const baseline = await createBaseline();
  const root = await createRepo({ dirtyBaseline: true });
  t.after(async () => {
    await rm(root, { recursive: true, force: true });
    await rm(baseline, { recursive: true, force: true });
  });

  const result = await checkWorktreeScope(root, baseline, path.join(baseline, "planned-paths.txt"));
  assert.equal(result.ok, false);
  assert.equal(result.exitCode, EXIT_MISMATCH);
  assert.deepEqual(result.mismatches.map(({ path: relPath }) => relPath), [".github/workflows/version-bump.yml"]);
  assert.match(formatFindings(result), /\[mismatch\] \.github\/workflows\/version-bump\.yml/);
});

test("rejects a malformed hash manifest", async (t) => {
  const baseline = await createBaseline({ malformedManifest: true });
  const root = await createRepo();
  t.after(async () => {
    await rm(root, { recursive: true, force: true });
    await rm(baseline, { recursive: true, force: true });
  });

  const result = await checkWorktreeScope(root, baseline, path.join(baseline, "planned-paths.txt"));
  assert.equal(result.ok, false);
  assert.equal(result.exitCode, EXIT_MISMATCH);
  assert.match(formatFindings(result), /nonplanned\.SHA256SUMS:1: malformed entry/);
});
