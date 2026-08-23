import { createHash } from "node:crypto";
import fs from "node:fs/promises";
import path from "node:path";

const EXIT_USAGE = 64;
const EXIT_FAILED = 65;

const PHASES = new Set(["pre", "post", "expected-old-path-failures"]);

function usage() {
  return "usage: node scripts/verify-frozen-evidence.mjs --root <root> --map <tsv> --stage <dir> --phase pre|post|expected-old-path-failures [--baseline <dir>]";
}

function parseArgs(argv) {
  const args = {};
  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];
    if (["--root", "--map", "--stage", "--phase", "--baseline"].includes(arg)) args[arg.slice(2)] = argv[++index];
    else throw Object.assign(new Error(`unknown argument: ${arg}\n${usage()}`), { exitCode: EXIT_USAGE });
  }
  if (!args.root || !args.map || !args.stage || !args.phase) throw Object.assign(new Error(usage()), { exitCode: EXIT_USAGE });
  if (!PHASES.has(args.phase)) throw Object.assign(new Error(`unknown phase: ${args.phase}\n${usage()}`), { exitCode: EXIT_USAGE });
  return args;
}

async function sha256File(absPath) {
  const hash = createHash("sha256");
  hash.update(await fs.readFile(absPath));
  return hash.digest("hex");
}

async function pathExists(absPath) {
  try { await fs.access(absPath); return true; } catch { return false; }
}

export async function readFrozenRows(mapPath) {
  const text = await fs.readFile(mapPath, "utf8");
  const lines = text.split(/\r?\n/).filter((line) => line.trim() !== "");
  const header = lines.shift();
  if (header !== "source\ttarget\tfrozen\tcontent-action") {
    throw Object.assign(new Error(`unexpected move-map header: ${header}`), { exitCode: EXIT_USAGE });
  }
  return lines.map((line) => {
    const [source, target, frozen, contentAction] = line.split("\t");
    return { source, target, frozen: frozen === "true", contentAction };
  }).filter((row) => row.frozen);
}

export function isManifest(row) {
  return /(?:^|\/)SHA256SUMS$|\.SHA256SUMS$/.test(row.source);
}

export function parseManifest(text) {
  const entries = [];
  for (const line of text.split(/\r?\n/)) {
    if (line.trim() === "") continue;
    const match = /^([0-9a-f]{64})  (.+)$/.exec(line);
    if (match) entries.push({ sha256: match[1], path: match[2] });
  }
  return entries;
}

// Base rule: manifests whose entries are all bare filenames verify from the
// manifest's own directory; manifests with repository-relative entries verify
// from the repository root of the staged tree under verification.
export function manifestBase(entries, manifestRel) {
  const basenameOnly = entries.length > 0 && entries.every((entry) => !entry.path.includes("/"));
  return basenameOnly ? path.posix.dirname(manifestRel) : "";
}

async function runManifest(rootAbs, manifestRel) {
  const manifestAbs = path.join(rootAbs, manifestRel);
  if (!(await pathExists(manifestAbs))) {
    return { manifest: manifestRel, error: "manifest-missing", total: 0, passed: 0, failed: [] };
  }
  const entries = parseManifest(await fs.readFile(manifestAbs, "utf8"));
  const base = manifestBase(entries, manifestRel);
  const failed = [];
  let passed = 0;
  for (const entry of entries) {
    const abs = path.join(rootAbs, base, entry.path);
    if (!(await pathExists(abs))) {
      failed.push({ path: entry.path, reason: "missing" });
      continue;
    }
    const actual = await sha256File(abs);
    if (actual === entry.sha256) passed += 1;
    else failed.push({ path: entry.path, reason: "hash-mismatch" });
  }
  return { manifest: manifestRel, base: base === "" ? "<repository-root>" : base, total: entries.length, passed, failed };
}

async function loadBaselineHashes(baselineAbs) {
  if (!baselineAbs) return new Map();
  const text = await fs.readFile(path.join(baselineAbs, "planned.SHA256SUMS"), "utf8");
  return new Map(parseManifest(text).map((entry) => [entry.path, entry.sha256]));
}

async function writeReceipt(stageAbs, name, receipt) {
  await fs.mkdir(stageAbs, { recursive: true });
  await fs.writeFile(path.join(stageAbs, name), `${JSON.stringify(receipt, null, 2)}\n`);
}

async function readReceipt(stageAbs, name) {
  return JSON.parse(await fs.readFile(path.join(stageAbs, name), "utf8"));
}

async function phasePre(args, rows) {
  const rootAbs = path.resolve(args.root);
  const baselineHashes = await loadBaselineHashes(args.baseline ? path.resolve(args.baseline) : null);
  const rowResults = [];
  let ok = true;
  for (const row of rows) {
    const abs = path.join(rootAbs, row.source);
    const exists = await pathExists(abs);
    if (!exists) {
      rowResults.push({ source: row.source, exists: false });
      ok = false;
      continue;
    }
    const sha256 = await sha256File(abs);
    const baseline = baselineHashes.has(row.source) ? baselineHashes.get(row.source) === sha256 : null;
    if (baseline === false) ok = false;
    rowResults.push({ source: row.source, exists: true, sha256, baselineMatch: baseline });
  }
  const manifests = [];
  for (const row of rows.filter(isManifest)) {
    manifests.push(await runManifest(rootAbs, row.source));
  }
  const receipt = { phase: "pre", ok, rows: rowResults, manifests, exitCode: ok ? 0 : EXIT_FAILED };
  await writeReceipt(path.resolve(args.stage), "frozen-pre-move.json", receipt);
  return { receipt, code: ok ? 0 : EXIT_FAILED };
}

async function phasePost(args, rows) {
  const rootAbs = path.resolve(args.root);
  const stageAbs = path.resolve(args.stage);
  const pre = await readReceipt(stageAbs, "frozen-pre-move.json");
  const preByRow = new Map(pre.rows.map((row) => [row.source, row]));
  const baselineHashes = await loadBaselineHashes(args.baseline ? path.resolve(args.baseline) : null);
  const rowResults = [];
  let ok = true;
  for (const row of rows) {
    const abs = path.join(rootAbs, row.target);
    const exists = await pathExists(abs);
    const preRow = preByRow.get(row.source);
    if (!exists) {
      rowResults.push({ source: row.source, target: row.target, exists: false });
      ok = false;
      continue;
    }
    const sha256 = await sha256File(abs);
    const preMatch = preRow?.sha256 === sha256;
    const baselineMatch = baselineHashes.has(row.source) ? baselineHashes.get(row.source) === sha256 : null;
    if (!preMatch || baselineMatch === false) ok = false;
    rowResults.push({ source: row.source, target: row.target, exists: true, sha256, preMoveMatch: preMatch, baselineMatch });
  }
  const manifests = [];
  for (const row of rows.filter(isManifest)) {
    manifests.push(await runManifest(rootAbs, row.target));
  }
  const receipt = { phase: "post", ok, rows: rowResults, manifests, exitCode: ok ? 0 : EXIT_FAILED };
  await writeReceipt(stageAbs, "frozen-post-content.json", receipt);
  return { receipt, code: ok ? 0 : EXIT_FAILED };
}

function failureKey(manifest, entry) {
  return `${manifest}::${entry.path}`;
}

async function phaseExpectedOldPathFailures(args, rows) {
  const stageAbs = path.resolve(args.stage);
  const pre = await readReceipt(stageAbs, "frozen-pre-move.json");
  const post = await readReceipt(stageAbs, "frozen-post-content.json");
  const manifestRows = rows.filter(isManifest);
  const postBySource = new Map();
  for (const row of manifestRows) {
    const found = post.manifests.find((manifest) => manifest.manifest === row.target);
    if (found) postBySource.set(row.source, found);
  }
  const rootManifests = new Set();
  const allowlist = [];
  for (const row of manifestRows) {
    const manifestAbs = path.join(path.resolve(args.root), row.target);
    if (!(await pathExists(manifestAbs))) continue;
    const entries = parseManifest(await fs.readFile(manifestAbs, "utf8"));
    if (manifestBase(entries, row.target) !== "") continue;
    rootManifests.add(row.source);
    for (const entry of entries) {
      if (entry.path.startsWith("doc/")) allowlist.push({ manifest: row.source, path: entry.path });
    }
  }
  const preFailures = new Map();
  const postFailures = new Map();
  for (const manifest of pre.manifests) {
    for (const failure of manifest.failed ?? []) preFailures.set(failureKey(manifest.manifest, failure), failure.reason);
  }
  for (const [source, manifest] of postBySource) {
    for (const failure of manifest.failed ?? []) postFailures.set(failureKey(source, failure), failure.reason);
  }
  const checks = [];
  let ok = true;

  // Every old-path entry in a repository-root manifest must fail post-move
  // with a missing-file error, because the bytes now live at docs/ targets.
  for (const item of allowlist) {
    const key = failureKey(item.manifest, { path: item.path });
    const reason = postFailures.get(key);
    const passed = reason === "missing";
    checks.push({ check: `old-path-absent:${item.manifest}:${item.path}`, ok: passed });
    ok &&= passed;
  }

  // Non-doc/ failures must be identical across the move (drift is historical,
  // not caused by the migration).
  const preNonDoc = new Set([...preFailures.keys()].filter((key) => !key.split("::")[1].startsWith("doc/")));
  const postNonDoc = new Set([...postFailures.keys()].filter((key) => !key.split("::")[1].startsWith("doc/")));
  const nonDocEqual = preNonDoc.size === postNonDoc.size && [...preNonDoc].every((key) => postNonDoc.has(key));
  checks.push({ check: "non-doc-failures-unchanged", ok: nonDocEqual, pre: [...preNonDoc], post: [...postNonDoc] });
  ok &&= nonDocEqual;

  // Basename manifests must behave identically at their new directory.
  const preByKey = new Map(pre.manifests.map((manifest) => [manifest.manifest, manifest]));
  for (const row of manifestRows) {
    if (rootManifests.has(row.source)) continue;
    const preManifest = preByKey.get(row.source);
    const postManifest = postBySource.get(row.source);
    const passed = (preManifest?.failed?.length ?? 0) === 0 && (postManifest?.failed?.length ?? 0) === 0;
    checks.push({ check: `basename-manifest-clean:${row.target}`, ok: passed });
    ok &&= passed;
  }

  const newFailures = [...postFailures.keys()].filter((key) => !preFailures.has(key));
  const receipt = { phase: "expected-old-path-failures", ok, allowlist, newFailures, checks, exitCode: ok ? 0 : EXIT_FAILED };
  await writeReceipt(stageAbs, "frozen-expected-old-path-failures.json", receipt);
  return { receipt, code: ok ? 0 : EXIT_FAILED };
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const rows = await readFrozenRows(path.resolve(args.root, args.map));
  if (args.phase === "pre") return (await phasePre(args, rows)).code;
  if (args.phase === "post") return (await phasePost(args, rows)).code;
  return (await phaseExpectedOldPathFailures(args, rows)).code;
}

const isDirectRun = process.argv[1] && import.meta.url === new URL(`file://${path.resolve(process.argv[1])}`).href;
if (isDirectRun) {
  main().then((code) => process.exit(code)).catch((error) => {
    console.error(error.message);
    process.exit(error.exitCode ?? 1);
  });
}
