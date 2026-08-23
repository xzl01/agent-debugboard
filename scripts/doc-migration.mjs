import { createHash } from "node:crypto";
import { constants, existsSync } from "node:fs";
import fs from "node:fs/promises";
import path from "node:path";

const EXIT_ABORTED = 70;
const EXIT_USAGE = 64;
const EXIT_DATA = 65;

// Live path-rewrite consumers (repository-relative). Every file is backed up
// into <stage>/rewrite-backup/ before any rewrite is applied.
const CONSUMERS = Object.freeze([
  "README.md",
  "README.zh-CN.md",
  "AGENTS.md",
  "web/README.md",
  "cmd-ng/README.md",
  "apps/radxa_linkr_debugger/README.md",
  "docs/user/openocd.md",
  "docs/user/openocd.zh-CN.md",
  "docs/user/install.md",
  "docs/user/install.zh-CN.md",
  "docs/user/logic-analyzer.md",
  "docs/user/logic-analyzer.zh-CN.md",
  "docs/user/power-analyzer.md",
  "docs/user/power-analyzer.zh-CN.md",
  "docs/user/board-overview.md",
  "docs/user/board-overview.zh-CN.md",
  "docs/developer/contributing.md",
  "docs/developer/contributing.zh-CN.md",
  "docs/developer/hardware-mapping.md",
  "docs/developer/hardware-mapping.zh-CN.md",
  "docs/developer/README.md",
  "docs/developer/README.zh-CN.md",
  "nix/package.nix",
  "scripts/check-repository-gates.mjs",
  "scripts/check-repository-gates.test.mjs",
  "scripts/check-persistent-configuration-docs.test.mjs",
  "scripts/persistent-configuration-docs/contracts.mjs",
  "scripts/persistent-configuration-docs/fixtures.mjs",
  "scripts/persistent-configuration-docs/validator.mjs",
  "scripts/persistent-configuration-docs/source-contract-mutations.mjs",
  "skills/radxa-linkr-debugger/SKILL.md",
]);

// Exact-string rules that the generic path/link engine cannot infer:
// repository-layout tree listings, synthetic fixture hrefs whose resolution
// base is the fixture key (not the module), and a synthetic missing-path
// literal used by a contract test.
const EXTRA_RULES = Object.freeze({
  "README.md": [
    ["doc/                          Hardware documents, schematics, and marketing assets",
     "docs/                         Documentation tree (user, developer, reference, testing, hardware, and assets)"],
  ],
  "README.zh-CN.md": [
    ["doc/                          硬件文档、原理图和宣传素材",
     "docs/                         文档树（用户、开发者、参考、测试、硬件与素材）"],
  ],
  "docs/developer/README.md": [
    ["doc/                          Hardware documents, schematics, and marketing assets",
     "docs/                         Documentation tree (user, developer, reference, testing, hardware, and assets)"],
  ],
  "docs/developer/README.zh-CN.md": [
    ["doc/                          硬件文档、原理图和营销素材",
     "docs/                         文档树（用户、开发者、参考、测试、硬件与素材）"],
  ],
  "scripts/check-persistent-configuration-docs.test.mjs": [
    ["doc/missing.md", "docs/reference/missing.md"],
  ],
  "scripts/persistent-configuration-docs/fixtures.mjs": [
    ["](testing/hil-functional-test-spec.md)", "](../testing/hil-functional-test-spec.md)"],
    ["](testing/results/2026-08-05-persistent-config-v1-save-hil.md)", "](../testing/results/2026-08-05-persistent-config-v1-save-hil.md)"],
    ["](testing/results/2026-07-30-persistent-config-hil.md)", "](../testing/results/2026-07-30-persistent-config-hil.md)"],
    ["[canonical](../persistent-configuration.md)", "[canonical](../reference/persistent-configuration.md)"],
  ],
});

const TEXT_ROW_EXTENSIONS = new Set([".md", ".cfg"]);
const RESIDUAL_DOC_PATTERN = /(^|[^A-Za-z0-9_/.-])((?:\.\.\/|\.\/)*)doc\//;

function usage() {
  return [
    "usage:",
    "  node scripts/doc-migration.mjs --root <root> --map <tsv> --stage <dir> --apply [--fail-after N] [--baseline <dir>]",
    "  node scripts/doc-migration.mjs --root <root> --map <tsv> --stage <dir> --rollback --journal <file> [--baseline <dir>]",
  ].join("\n");
}

function parseArgs(argv) {
  const args = { failAfter: null };
  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];
    if (arg === "--apply") args.apply = true;
    else if (arg === "--rollback") args.rollback = true;
    else if (arg === "--fail-after") { args.failAfter = Number(argv[++index]); }
    else if (["--root", "--map", "--stage", "--journal", "--baseline"].includes(arg)) args[arg.slice(2).replace(/-([a-z])/g, (_match, c) => c.toUpperCase())] = argv[++index];
    else throw Object.assign(new Error(`unknown argument: ${arg}`), { exitCode: EXIT_USAGE });
  }
  if (!args.root || !args.map || !args.stage) throw Object.assign(new Error(usage()), { exitCode: EXIT_USAGE });
  if (args.apply === args.rollback) throw Object.assign(new Error(`exactly one of --apply or --rollback is required\n${usage()}`), { exitCode: EXIT_USAGE });
  if (args.rollback && !args.journal) throw Object.assign(new Error("--rollback requires --journal <file>"), { exitCode: EXIT_USAGE });
  if (args.failAfter !== null && (!Number.isInteger(args.failAfter) || args.failAfter < 1)) {
    throw Object.assign(new Error("--fail-after requires a positive integer"), { exitCode: EXIT_USAGE });
  }
  return args;
}

async function sha256File(absPath) {
  const hash = createHash("sha256");
  hash.update(await fs.readFile(absPath));
  return hash.digest("hex");
}

async function pathExists(absPath) {
  try { await fs.access(absPath, constants.F_OK); return true; } catch { return false; }
}

async function readMoveMap(mapPath) {
  const text = await fs.readFile(mapPath, "utf8");
  const lines = text.split(/\r?\n/).filter((line) => line.trim() !== "");
  const header = lines.shift();
  if (header !== "source\ttarget\tfrozen\tcontent-action") {
    throw Object.assign(new Error(`unexpected move-map header: ${header}`), { exitCode: EXIT_DATA });
  }
  const rows = lines.map((line, index) => {
    const [source, target, frozen, contentAction] = line.split("\t");
    if (!source || !target || !["true", "false"].includes(frozen) || !contentAction) {
      throw Object.assign(new Error(`move-map row ${index + 2} is malformed: ${line}`), { exitCode: EXIT_DATA });
    }
    return { source, target, frozen: frozen === "true", contentAction };
  });
  const targets = new Set();
  for (const row of rows) {
    if (!row.source.startsWith("doc/")) throw Object.assign(new Error(`row source escapes doc/: ${row.source}`), { exitCode: EXIT_DATA });
    if (!row.target.startsWith("docs/")) throw Object.assign(new Error(`row target escapes docs/: ${row.target}`), { exitCode: EXIT_DATA });
    if (targets.has(row.target)) throw Object.assign(new Error(`duplicate move target: ${row.target}`), { exitCode: EXIT_DATA });
    targets.add(row.target);
  }
  return rows;
}

async function listFilesRecursive(absDir, prefix, out = []) {
  const entries = await fs.readdir(absDir, { withFileTypes: true });
  for (const entry of entries) {
    const rel = prefix === "" ? entry.name : `${prefix}/${entry.name}`;
    if (entry.isDirectory()) await listFilesRecursive(path.join(absDir, entry.name), rel, out);
    else if (entry.isFile()) out.push(rel);
  }
  return out;
}

async function copyTree(absSource, absTarget) {
  await fs.mkdir(absTarget, { recursive: true });
  const entries = await fs.readdir(absSource, { withFileTypes: true });
  for (const entry of entries) {
    const from = path.join(absSource, entry.name);
    const to = path.join(absTarget, entry.name);
    if (entry.isDirectory()) await copyTree(from, to);
    else if (entry.isFile()) await fs.copyFile(from, to, constants.COPYFILE_EXCL);
  }
}

function escapeRegExp(text) {
  return text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

// Exact per-row path rules (longest source first) plus consistent directory
// prefix rules (longest prefix first). A directory prefix is only emitted
// when every row below it maps into one corresponding target directory.
function buildPathRules(rows) {
  const exact = rows
    .map((row) => ({ from: row.source, to: row.target, re: new RegExp(`(?<!share/)${escapeRegExp(row.source)}(?![/A-Za-z0-9_-])`, "g") }))
    .sort((a, b) => b.from.length - a.from.length);
  const byDir = new Map();
  for (const row of rows) {
    const dir = path.posix.dirname(row.source);
    if (dir === "doc") continue;
    if (!byDir.has(dir)) byDir.set(dir, []);
    byDir.get(dir).push(row);
  }
  const prefixes = [];
  for (const [dir, dirRows] of byDir) {
    const targetDir = path.posix.dirname(dirRows[0].target);
    const consistent = dirRows.every((row) => row.target === `${targetDir}/${row.source.slice(dir.length + 1)}`);
    if (consistent) prefixes.push({ from: `${dir}/`, to: `${targetDir}/`, re: new RegExp(`(?<!share/)${escapeRegExp(`${dir}/`)}`, "g") });
  }
  prefixes.sort((a, b) => b.from.length - a.from.length);
  return [...exact, ...prefixes];
}

function applyPathRules(content, rules) {
  let replacements = 0;
  let next = content;
  for (const rule of rules) {
    next = next.replace(rule.re, () => { replacements += 1; return rule.to; });
  }
  return { content: next, replacements };
}

// Recalculate every local Markdown link against the referencing file's old
// and new directory. Links resolving to a moved path are retargeted; links
// resolving to real unmoved paths are re-expressed relative to the new
// directory. Links that resolve to nothing (pre-existing broken or synthetic
// text) are left untouched.
function recalculateMarkdownLinks(content, oldDir, newDir, targetBySource, existsRel) {
  let replacements = 0;
  const next = content.replace(/\]\(([^)\s#]+)(#[^)]*)?\)/g, (whole, href, anchor = "") => {
    if (/^(?:[a-z][a-z0-9+.-]*:|\/)/i.test(href)) return whole;
    const resolvedOld = path.posix.normalize(path.posix.join(oldDir, href));
    const mapped = targetBySource.get(resolvedOld);
    const finalTarget = mapped ?? resolvedOld;
    if (mapped === undefined && !existsRel(resolvedOld)) return whole;
    const rebased = path.posix.relative(newDir, finalTarget);
    if (rebased === "" || rebased === href) return whole;
    replacements += 1;
    return `](${rebased}${anchor})`;
  });
  return { content: next, replacements };
}

function rewriteContent(relPath, content, rules, oldDir, newDir, targetBySource, existsRel) {
  let total = 0;
  let next = content;
  const phaseA = applyPathRules(next, rules);
  next = phaseA.content;
  total += phaseA.replacements;
  if (relPath.endsWith(".md")) {
    const phaseB = recalculateMarkdownLinks(next, oldDir, newDir, targetBySource, existsRel);
    next = phaseB.content;
    total += phaseB.replacements;
  }
  for (const [from, to] of EXTRA_RULES[relPath] ?? []) {
    const occurrences = next.split(from).length - 1;
    if (occurrences > 0) {
      next = next.split(from).join(to);
      total += occurrences;
    }
  }
  if (RESIDUAL_DOC_PATTERN.test(next)) {
    const line = next.split(/\r?\n/).findIndex((row) => RESIDUAL_DOC_PATTERN.test(row));
    throw Object.assign(new Error(`residual doc/ reference remains in ${relPath} at line ${line + 1}`), { exitCode: EXIT_DATA });
  }
  return { content: next, replacements: total };
}

async function readManifestEntries(manifestPath) {
  const text = await fs.readFile(manifestPath, "utf8");
  const entries = [];
  for (const line of text.split(/\r?\n/)) {
    if (line.trim() === "") continue;
    const match = /^([0-9a-f]{64})  (.+)$/.exec(line);
    if (match) entries.push({ sha256: match[1], path: match[2] });
  }
  return entries;
}

async function checkBaselineManifest(manifestPath, rootAbs, tolerateOmo) {
  const entries = await readManifestEntries(manifestPath);
  const failures = [];
  let checked = 0;
  for (const entry of entries) {
    const abs = path.join(rootAbs, entry.path);
    checked += 1;
    if (!(await pathExists(abs))) {
      failures.push({ path: entry.path, reason: "missing" });
      continue;
    }
    const actual = await sha256File(abs);
    if (actual !== entry.sha256) failures.push({ path: entry.path, reason: "hash-mismatch" });
  }
  const hard = failures.filter((failure) => !(tolerateOmo && failure.path.startsWith(".omo/")));
  const tolerated = failures.filter((failure) => tolerateOmo && failure.path.startsWith(".omo/"));
  return { manifest: path.basename(manifestPath), checked, failures, hard, tolerated, ok: hard.length === 0 };
}

class ActionJournal {
  constructor(stageAbs) {
    this.stageAbs = stageAbs;
    this.actions = [];
    this.seq = 0;
  }
  record(action) {
    this.seq += 1;
    this.actions.push({ seq: this.seq, ...action });
    return this.seq;
  }
  async flush(file) {
    await fs.mkdir(path.dirname(file), { recursive: true });
    await fs.writeFile(file, `${JSON.stringify(this.snapshot("in-progress"), null, 2)}\n`);
  }
  snapshot(status) {
    return { version: 1, stage: this.stageAbs, status, actions: this.actions };
  }
}

async function runApply(args) {
  const rootAbs = path.resolve(args.root);
  const stageAbs = path.resolve(args.stage);
  const rows = await readMoveMap(path.resolve(rootAbs, args.map));
  const targetBySource = new Map(rows.map((row) => [row.source, row.target]));
  const rules = buildPathRules(rows);

  const docAbs = path.join(rootAbs, "doc");
  if (!(await pathExists(docAbs))) throw Object.assign(new Error("doc/ does not exist; nothing to apply"), { exitCode: EXIT_DATA });
  const inventory = (await listFilesRecursive(docAbs, "doc")).sort();
  const mapped = rows.map((row) => row.source).sort();
  if (JSON.stringify(inventory) !== JSON.stringify(mapped)) {
    const onlyOnDisk = inventory.filter((item) => !mapped.includes(item));
    const onlyInMap = mapped.filter((item) => !inventory.includes(item));
    throw Object.assign(new Error(`doc/ inventory mismatch; on-disk-only=${JSON.stringify(onlyOnDisk)} map-only=${JSON.stringify(onlyInMap)}`), { exitCode: EXIT_DATA });
  }
  for (const row of rows) {
    if (await pathExists(path.join(rootAbs, row.target))) {
      throw Object.assign(new Error(`target already exists: ${row.target}; roll back before re-applying`), { exitCode: EXIT_DATA });
    }
  }
  for (const consumer of CONSUMERS) {
    if (!(await pathExists(path.join(rootAbs, consumer)))) {
      throw Object.assign(new Error(`rewrite consumer missing: ${consumer}`), { exitCode: EXIT_DATA });
    }
  }

  let baselineCheck = null;
  if (args.baseline) {
    const plannedPath = path.join(path.resolve(args.baseline), "planned.SHA256SUMS");
    const planned = await readManifestEntries(plannedPath);
    const wanted = new Set([...rows.map((row) => row.source), ...CONSUMERS]);
    const mismatches = [];
    for (const entry of planned) {
      if (!wanted.has(entry.path)) continue;
      const abs = path.join(rootAbs, entry.path);
      if (!(await pathExists(abs)) || (await sha256File(abs)) !== entry.sha256) mismatches.push(entry.path);
    }
    baselineCheck = { manifest: "planned.SHA256SUMS", compared: wanted.size, mismatches };
    if (mismatches.length > 0) {
      throw Object.assign(new Error(`baseline drift detected before apply: ${JSON.stringify(mismatches)}`), { exitCode: EXIT_DATA });
    }
  }

  await fs.mkdir(stageAbs, { recursive: true });
  const stageSource = path.join(stageAbs, "source");
  if (await pathExists(stageSource)) throw Object.assign(new Error(`stage source already exists: ${stageSource}`), { exitCode: EXIT_DATA });
  await copyTree(docAbs, stageSource);

  const journal = new ActionJournal(stageAbs);
  const journalPath = path.join(stageAbs, "journal.json");
  const rewriteBackup = path.join(stageAbs, "rewrite-backup");

  const rewrites = [];
  for (const consumer of CONSUMERS) {
    rewrites.push({ rel: consumer, oldDir: path.posix.dirname(consumer), newDir: path.posix.dirname(consumer) });
  }
  for (const row of rows) {
    if (row.frozen || !TEXT_ROW_EXTENSIONS.has(path.posix.extname(row.source))) continue;
    rewrites.push({ rel: row.source, oldDir: path.posix.dirname(row.source), newDir: path.posix.dirname(row.target) });
  }

  const rollbackCommand = `node scripts/doc-migration.mjs --root ${args.root} --map ${args.map} --stage ${args.stage} --rollback --journal ${path.relative(rootAbs, journalPath)}`;
  const targetSet = new Set(rows.map((row) => row.target));
  const existsRel = (rel) => targetSet.has(rel) || existsSync(path.join(rootAbs, rel));
  let aborted = null;

  const execute = async () => {
    for (const item of rewrites) {
      const abs = path.join(rootAbs, item.rel);
      const before = await fs.readFile(abs);
      const rewritten = rewriteContent(item.rel, before.toString("utf8"), rules, item.oldDir, item.newDir, targetBySource, existsRel);
      if (rewritten.content === before.toString("utf8")) continue;
      const shaBefore = createHash("sha256").update(before).digest("hex");
      const backupAbs = path.join(rewriteBackup, item.rel);
      await fs.mkdir(path.dirname(backupAbs), { recursive: true });
      await fs.copyFile(abs, backupAbs);
      await fs.writeFile(abs, rewritten.content);
      const shaAfter = await sha256File(abs);
      journal.record({ type: "rewrite", path: item.rel, backup: path.relative(stageAbs, backupAbs), sha256Before: shaBefore, sha256After: shaAfter, replacements: rewritten.replacements });
      if (journal.seq === args.failAfter) { aborted = `fail-after ${args.failAfter}`; return; }
    }
    for (const row of rows) {
      const sourceAbs = path.join(rootAbs, row.source);
      const targetAbs = path.join(rootAbs, row.target);
      const sha = await sha256File(sourceAbs);
      await fs.mkdir(path.dirname(targetAbs), { recursive: true });
      await fs.rename(sourceAbs, targetAbs);
      const movedSha = await sha256File(targetAbs);
      if (movedSha !== sha) throw new Error(`byte mismatch after move: ${row.source} -> ${row.target}`);
      journal.record({ type: "move", source: row.source, target: row.target, sha256: sha });
      if (journal.seq === args.failAfter) { aborted = `fail-after ${args.failAfter}`; return; }
    }
    const dirs = (await listDirsRecursive(docAbs, "doc")).sort((a, b) => b.length - a.length || b.split("/").length - a.split("/").length);
    for (const dir of dirs) {
      const abs = path.join(rootAbs, dir);
      const remaining = await fs.readdir(abs);
      if (remaining.length === 0) {
        await fs.rmdir(abs);
        journal.record({ type: "rmdir", path: dir });
        if (journal.seq === args.failAfter) { aborted = `fail-after ${args.failAfter}`; return; }
      }
    }
  };

  let failure = null;
  try {
    await execute();
  } catch (error) {
    failure = error;
  }

  const journalFile = journal.snapshot(aborted ? "aborted" : failure ? "failed" : "applied");
  await fs.mkdir(stageAbs, { recursive: true });
  await fs.writeFile(journalPath, `${JSON.stringify(journalFile, null, 2)}\n`);

  if (failure) {
    let autoRollback = "not-attempted";
    try {
      await runRollback({ ...args, journal: path.relative(rootAbs, journalPath) }, journalFile);
      autoRollback = "completed";
    } catch (rollbackError) {
      autoRollback = `failed: ${rollbackError.message}`;
    }
    const receipt = { status: "failed", error: failure.message, autoRollback, actions: journal.seq, rollbackCommand, baselineCheck };
    await fs.writeFile(path.join(stageAbs, "apply.json"), `${JSON.stringify(receipt, null, 2)}\n`);
    console.error(`apply failed: ${failure.message}; auto-rollback: ${autoRollback}`);
    return EXIT_ABORTED;
  }
  if (aborted) {
    const receipt = { status: "aborted", reason: aborted, actions: journal.seq, rollbackCommand, baselineCheck };
    await fs.writeFile(path.join(stageAbs, "apply.json"), `${JSON.stringify(receipt, null, 2)}\n`);
    console.error(`apply aborted after ${journal.seq} journaled actions (${aborted}). Roll back with:\n  ${rollbackCommand}`);
    return EXIT_ABORTED;
  }

  const receipt = {
    status: "applied",
    actions: journal.seq,
    rewrites: journal.actions.filter((action) => action.type === "rewrite").length,
    moves: journal.actions.filter((action) => action.type === "move").length,
    rmdirs: journal.actions.filter((action) => action.type === "rmdir").length,
    docRemoved: !(await pathExists(docAbs)),
    rollbackCommand,
    baselineCheck,
  };
  await fs.writeFile(path.join(stageAbs, "apply.json"), `${JSON.stringify(receipt, null, 2)}\n`);
  return 0;
}

async function listDirsRecursive(absDir, prefix, out = []) {
  const entries = await fs.readdir(absDir, { withFileTypes: true });
  for (const entry of entries) {
    if (!entry.isDirectory()) continue;
    const rel = prefix === "" ? entry.name : `${prefix}/${entry.name}`;
    out.push(rel);
    await listDirsRecursive(path.join(absDir, entry.name), rel, out);
  }
  if (prefix === "doc") out.push("doc");
  return out;
}

async function runRollback(args, preloadedJournal = null) {
  const rootAbs = path.resolve(args.root);
  const stageAbs = path.resolve(args.stage);
  const journalPath = preloadedJournal ? path.join(stageAbs, "journal.json") : path.resolve(rootAbs, args.journal);
  const journal = preloadedJournal ?? JSON.parse(await fs.readFile(journalPath, "utf8"));
  const restored = [];
  const skipped = [];
  const ordered = [...journal.actions].sort((a, b) => b.seq - a.seq);
  const rewriteOriginal = new Map();
  for (const action of journal.actions) {
    if (action.type === "rewrite") rewriteOriginal.set(action.path, action.sha256Before);
  }
  for (const action of ordered) {
    if (action.type === "rmdir") {
      const abs = path.join(rootAbs, action.path);
      if (!(await pathExists(abs))) {
        await fs.mkdir(abs, { recursive: true });
        restored.push(action);
      } else skipped.push({ ...action, reason: "directory already present" });
    } else if (action.type === "move") {
      const sourceAbs = path.join(rootAbs, action.source);
      const targetAbs = path.join(rootAbs, action.target);
      if (await pathExists(sourceAbs)) {
        const sha = await sha256File(sourceAbs);
        if (sha === action.sha256 || sha === rewriteOriginal.get(action.source)) { skipped.push({ ...action, reason: "source already restored" }); continue; }
        throw new Error(`rollback conflict: ${action.source} exists with unexpected content`);
      }
      if (!(await pathExists(targetAbs))) throw new Error(`rollback missing moved file: ${action.target}`);
      const sha = await sha256File(targetAbs);
      if (sha !== action.sha256) throw new Error(`rollback byte mismatch: ${action.target}`);
      await fs.mkdir(path.dirname(sourceAbs), { recursive: true });
      await fs.rename(targetAbs, sourceAbs);
      restored.push(action);
    } else if (action.type === "rewrite") {
      const abs = path.join(rootAbs, action.path);
      const backupAbs = path.join(stageAbs, action.backup);
      if (!(await pathExists(backupAbs))) throw new Error(`rollback missing rewrite backup: ${action.backup}`);
      if (await pathExists(abs)) {
        const current = await sha256File(abs);
        if (current === action.sha256Before) { skipped.push({ ...action, reason: "content already restored" }); continue; }
        if (current !== action.sha256After) throw new Error(`rollback conflict: ${action.path} drifted since apply`);
      }
      await fs.mkdir(path.dirname(abs), { recursive: true });
      await fs.copyFile(backupAbs, abs);
      const sha = await sha256File(abs);
      if (sha !== action.sha256Before) throw new Error(`rollback restore mismatch: ${action.path}`);
      restored.push(action);
    }
  }

  const verification = [];
  let ok = true;
  for (const action of journal.actions) {
    if (action.type === "move") {
      const abs = path.join(rootAbs, action.source);
      // A file that was rewritten before its move must end up with its
      // pre-rewrite bytes; all other moved files keep their move-time bytes.
      const expected = rewriteOriginal.get(action.source) ?? action.sha256;
      const present = (await pathExists(abs)) && (await sha256File(abs)) === expected;
      const targetGone = !(await pathExists(path.join(rootAbs, action.target)));
      verification.push({ check: `move-restored:${action.source}`, ok: present && targetGone });
      ok &&= present && targetGone;
    } else if (action.type === "rewrite") {
      const abs = path.join(rootAbs, action.path);
      const present = (await pathExists(abs)) && (await sha256File(abs)) === action.sha256Before;
      verification.push({ check: `rewrite-restored:${action.path}`, ok: present });
      ok &&= present;
    }
  }

  const manifestChecks = [];
  if (args.baseline) {
    const baselineAbs = path.resolve(args.baseline);
    for (const name of ["planned.SHA256SUMS", "nonplanned.SHA256SUMS"]) {
      const result = await checkBaselineManifest(path.join(baselineAbs, name), rootAbs, true);
      manifestChecks.push({ ...result, failures: undefined, hardFailures: result.hard, toleratedFailures: result.tolerated });
      ok &&= result.ok;
    }
  }

  const receipt = { status: ok ? "rolled-back" : "rollback-incomplete", restored: restored.length, skipped: skipped.length, verification: verification.filter((item) => !item.ok), manifestChecks };
  await fs.writeFile(path.join(stageAbs, "rollback.json"), `${JSON.stringify(receipt, null, 2)}\n`);
  if (!ok) throw new Error(`rollback verification failed: ${JSON.stringify(receipt.verification)}`);
  return 0;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.apply) return await runApply(args);
  return await runRollback(args);
}

main().then((code) => process.exit(code)).catch((error) => {
  console.error(error.message);
  process.exit(error.exitCode ?? 1);
});
