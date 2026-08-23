import { execFileSync } from "node:child_process";
import fs from "node:fs/promises";
import path from "node:path";

const EXIT_USAGE = 64;
const EXIT_VIOLATIONS = 65;

// Tooling that defines the doc/ -> docs/ contract contains the legacy pattern
// by design and is exempt from the scan.
const TOOLING_EXEMPT = new Set([
  "scripts/doc-migration.mjs",
  "scripts/check-doc-layout.mjs",
  "scripts/check-doc-layout.test.mjs",
  "scripts/verify-frozen-evidence.mjs",
  "scripts/verify-frozen-evidence.test.mjs",
]);

const BINARY_EXTENSION = /\.(?:png|pdf|uf2|tar|gz|zip|jpe?g|gif|ico|wasm|bin)$/i;
const FROZEN_EVIDENCE = /^docs\/testing\/(?:results|reports)\//;

// Matches a legacy top-level doc/ reference: at a boundary (not after a word
// character, slash, dot, or hyphen), with optional ./ or ../ segments, then
// doc/. This deliberately does not match $out/share/doc/ install
// destinations, jsdoc/, typedoc/, or docs/ itself.
const LEGACY_DOC_PATTERN = /(^|[^A-Za-z0-9_/.-])((?:\.\.\/|\.\/)*)doc\//;

function usage() {
  return "usage: node scripts/check-doc-layout.mjs --root <repository-root> [--files <newline-list>] [--format text|json]";
}

function parseArgs(argv) {
  const args = { format: "text" };
  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];
    if (["--root", "--files", "--format"].includes(arg)) args[arg.slice(2)] = argv[++index];
    else throw Object.assign(new Error(`unknown argument: ${arg}\n${usage()}`), { exitCode: EXIT_USAGE });
  }
  if (!args.root) throw Object.assign(new Error(usage()), { exitCode: EXIT_USAGE });
  if (!["text", "json"].includes(args.format)) throw Object.assign(new Error(`--format must be text or json\n${usage()}`), { exitCode: EXIT_USAGE });
  return args;
}

function gitListFiles(root, args) {
  const out = execFileSync("git", ["ls-files", ...args, "-z"], { cwd: root, encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
  return out.split("\0").filter((entry) => entry !== "");
}

export function isExcluded(relPath) {
  if (relPath === ".git" || relPath.startsWith(".git/")) return true;
  if (relPath.startsWith(".omo/")) return true;
  if (relPath === "build" || relPath.startsWith("build/")) return true;
  if (relPath.includes("node_modules/") || relPath.startsWith("node_modules/")) return true;
  if (relPath.startsWith("web/dist/")) return true;
  if (/(^|\/)target(?:\/|$)/.test(relPath)) return true;
  if (relPath.endsWith(".tsbuildinfo")) return true;
  if (TOOLING_EXEMPT.has(relPath)) return true;
  if (BINARY_EXTENSION.test(relPath)) return true;
  return false;
}

export function findLegacyReferences(content) {
  const violations = [];
  const lines = content.split(/\r?\n/);
  for (let index = 0; index < lines.length; index += 1) {
    if (LEGACY_DOC_PATTERN.test(lines[index])) violations.push({ line: index + 1, text: lines[index].trim().slice(0, 200) });
  }
  return violations;
}

async function collectCandidates(root) {
  const trackedAndUntracked = gitListFiles(root, ["-co", "--exclude-standard"]);
  const ignored = gitListFiles(root, ["-o", "-i", "--exclude-standard"]);
  return [...new Set([...trackedAndUntracked, ...ignored])].sort();
}

export async function checkDocLayout(rootPath, explicitFiles = null) {
  const root = path.resolve(rootPath);
  const candidates = explicitFiles ?? (await collectCandidates(root));
  const violations = [];
  const skipped = { excluded: 0, frozen: 0, missing: 0, nonRegular: 0 };
  let scanned = 0;
  for (const rel of candidates) {
    if (typeof rel !== "string" || rel === "" || path.isAbsolute(rel) || rel.startsWith("..")) continue;
    if (isExcluded(rel)) { skipped.excluded += 1; continue; }
    if (FROZEN_EVIDENCE.test(rel)) { skipped.frozen += 1; continue; }
    const abs = path.join(root, rel);
    let stat;
    try {
      stat = await fs.stat(abs);
    } catch {
      skipped.missing += 1;
      continue;
    }
    if (!stat.isFile()) { skipped.nonRegular += 1; continue; }
    let content;
    try {
      content = await fs.readFile(abs, "utf8");
    } catch {
      skipped.nonRegular += 1;
      continue;
    }
    scanned += 1;
    for (const hit of findLegacyReferences(content)) {
      violations.push({ path: rel, line: hit.line, text: hit.text });
    }
  }
  return { ok: violations.length === 0, scanned, skipped, violations };
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  let explicitFiles = null;
  if (args.files) {
    const list = await fs.readFile(path.resolve(args.files), "utf8");
    explicitFiles = list.split(/\r?\n/).filter((entry) => entry.trim() !== "");
  }
  const result = await checkDocLayout(args.root, explicitFiles);
  if (args.format === "json") {
    console.log(JSON.stringify(result, null, 2));
  } else if (result.ok) {
    console.log(`doc-layout OK: ${result.scanned} files scanned, no legacy doc/ references`);
  } else {
    console.error(`doc-layout violations (${result.violations.length}):`);
    for (const violation of result.violations) {
      console.error(`- ${violation.path}:${violation.line}: ${violation.text}`);
    }
  }
  return result.ok ? 0 : EXIT_VIOLATIONS;
}

const isDirectRun = process.argv[1] && import.meta.url === new URL(`file://${path.resolve(process.argv[1])}`).href;
if (isDirectRun) {
  main().then((code) => process.exit(code)).catch((error) => {
    console.error(error.message);
    process.exit(error.exitCode ?? 1);
  });
}
