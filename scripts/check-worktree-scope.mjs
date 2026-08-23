import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

export const EXIT_USAGE = 64;
export const EXIT_UNKNOWN_ADDITION = 65;
export const EXIT_MISSING_PATH = 66;
export const EXIT_MISMATCH = 67;

const MAX_BUFFER = 128 * 1024 * 1024;
const DEFAULT_GENERATED_EXCLUSIONS = ["web/node_modules/**", "web/dist/**", "build/**", "**/target/**", ".git/**", ".omo/**"];
const HASH_MANIFEST = "nonplanned.SHA256SUMS";

function usage() {
  return "usage: node scripts/check-worktree-scope.mjs --root <repository-root> --baseline <baseline-dir> --planned <planned-paths.txt>";
}

function parseArgs(argv) {
  const args = {};
  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];
    if (["--root", "--baseline", "--planned"].includes(arg)) args[arg.slice(2)] = argv[++index];
    else throw Object.assign(new Error(`unknown argument: ${arg}\n${usage()}`), { exitCode: EXIT_USAGE });
  }
  if (!args.root || !args.baseline || !args.planned) throw Object.assign(new Error(usage()), { exitCode: EXIT_USAGE });
  return args;
}

async function readOptionalText(filePath) {
  try {
    return await readFile(filePath, "utf8");
  } catch (error) {
    if (error?.code === "ENOENT") return null;
    throw error;
  }
}

function addLines(set, text) {
  for (const line of text.split(/\r?\n/)) {
    const trimmed = line.trim();
    if (trimmed !== "") set.add(trimmed);
  }
}

function splitNulPaths(buffer) {
  return buffer.toString("utf8").split("\0").filter((entry) => entry !== "");
}

function gitPaths(root, args) {
  return splitNulPaths(execFileSync("git", args, { cwd: root, encoding: "buffer", maxBuffer: MAX_BUFFER }));
}

function gitPorcelain(root) {
  return splitNulPaths(execFileSync("git", ["status", "--porcelain=v1", "-z", "--untracked-files=all"], { cwd: root, encoding: "buffer", maxBuffer: MAX_BUFFER }));
}

function globToRegExp(pattern) {
  const escaped = pattern.replace(/[\\^$.*+?()[\]{}|]/g, "\\$&");
  return new RegExp(`^${escaped.replace(/\\\*\\\*/g, ".*").replace(/\\\*/g, "[^/]*")}$`);
}

async function loadGeneratedRules(baselineDir) {
  const schema = await readOptionalText(path.join(baselineDir, "planned-paths.schema.tsv"));
  const source = schema?.split(/\r?\n/).find((line) => line.startsWith("generated_exclusions\t"))?.split("\t")[1];
  const rawPatterns = (source ?? DEFAULT_GENERATED_EXCLUSIONS.join(", ")).split(",");
  return rawPatterns.map((entry) => entry.trim().split(/\s+/)[0]).filter((entry) => entry !== "").map(globToRegExp);
}

function isSafeManifestPath(relPath) {
  return relPath !== "" && !path.posix.isAbsolute(relPath) && !relPath.startsWith("../") && relPath !== ".." && path.posix.normalize(relPath) === relPath;
}

function parseSha256Manifest(text) {
  const entries = new Map();
  const errors = [];
  const lines = text.split(/\r?\n/);
  for (const [index, line] of lines.entries()) {
    if (line.trim() === "") continue;
    const match = /^([a-fA-F0-9]{64})(?:  | \*)(.+)$/.exec(line);
    if (!match) {
      errors.push(`${HASH_MANIFEST}:${index + 1}: malformed entry`);
      continue;
    }
    const [, hash, relPath] = match;
    if (!isSafeManifestPath(relPath)) {
      errors.push(`${HASH_MANIFEST}:${index + 1}: unsafe path ${relPath}`);
      continue;
    }
    if (entries.has(relPath)) {
      errors.push(`${HASH_MANIFEST}:${index + 1}: duplicate path ${relPath}`);
      continue;
    }
    entries.set(relPath, hash.toLowerCase());
  }
  return { entries, errors };
}

async function loadSha256Manifest(baselineDir) {
  const text = await readOptionalText(path.join(baselineDir, HASH_MANIFEST));
  if (text === null) return { entries: new Map(), errors: [`${HASH_MANIFEST}: missing`] };
  return parseSha256Manifest(text);
}

function isContentExempt(relPath, planned, generatedRules) {
  if (relPath === ".omo/evidence" || relPath.startsWith(".omo/evidence/")) return true;
  if (planned.has(relPath)) return true;
  return generatedRules.some((rule) => rule.test(relPath));
}

async function collectContentMismatches(root, current, planned, generatedRules, manifestEntries) {
  const mismatches = [];
  for (const [relPath, expectedHash] of manifestEntries) {
    if (isContentExempt(relPath, planned, generatedRules) || !current.has(relPath)) continue;
    try {
      const actualHash = createHash("sha256").update(await readFile(path.join(root, relPath))).digest("hex");
      if (actualHash !== expectedHash) mismatches.push({ path: relPath, expectedHash, actualHash });
    } catch (error) {
      mismatches.push({ path: relPath, expectedHash, actualHash: null, error: error?.code ?? String(error) });
    }
  }
  return mismatches;
}

async function loadPreexistingDeletedPaths(baselineDir) {
  const deleted = new Set();
  const text = await readOptionalText(path.join(baselineDir, "preexisting-deleted-paths.tsv"));
  if (text === null) return deleted;
  for (const line of text.split(/\r?\n/)) {
    if (line.trim() === "") continue;
    const [, relPath] = line.split("\t");
    if (relPath) deleted.add(relPath);
  }
  return deleted;
}

function loadUnstagedDeletedPaths(root) {
  const deleted = new Set();
  for (const entry of gitPorcelain(root)) {
    if (entry.length < 4) continue;
    if (entry[1] !== "D" || entry[0] === "D") continue;
    deleted.add(entry.slice(3));
  }
  return deleted;
}

async function loadBaselineKnownPaths(baselineDir) {
  const known = new Set();
  const universe = await readOptionalText(path.join(baselineDir, "universe.paths"));
  if (universe !== null) {
    addLines(known, universe);
    return known;
  }
  for (const relative of ["planned-paths.current", "planned-paths.candidates", "planned-missing.paths", "live-doc-reference.paths"]) {
    const text = await readOptionalText(path.join(baselineDir, relative));
    if (text !== null) addLines(known, text);
  }
  const categories = await readOptionalText(path.join(baselineDir, "planned-paths-with-categories.tsv"));
  if (categories !== null) {
    for (const line of categories.split(/\r?\n/)) {
      if (line.trim() === "") continue;
      const [, rel] = line.split("\t");
      if (rel) known.add(rel);
    }
  }
  return known;
}

async function loadPlannedPaths(plannedFile) {
  const planned = await readOptionalText(plannedFile);
  if (planned === null) throw Object.assign(new Error(`planned path list missing: ${plannedFile}`), { exitCode: EXIT_MISMATCH });
  const allowed = new Set();
  addLines(allowed, planned);
  return allowed;
}

function isAllowedPath(relPath, planned, known, generatedRules) {
  if (relPath === ".omo/evidence" || relPath.startsWith(".omo/evidence/")) return true;
  if (planned.has(relPath) || known.has(relPath)) return true;
  return generatedRules.some((rule) => rule.test(relPath));
}

export async function checkWorktreeScope(repositoryRoot, baselineDir, plannedFile) {
  const root = path.resolve(repositoryRoot);
  const planned = await loadPlannedPaths(path.resolve(plannedFile));
  const known = await loadBaselineKnownPaths(path.resolve(baselineDir));
  const generatedRules = await loadGeneratedRules(path.resolve(baselineDir));
  const hashManifest = await loadSha256Manifest(path.resolve(baselineDir));
  const preexistingDeleted = await loadPreexistingDeletedPaths(path.resolve(baselineDir));
  const unstagedDeleted = loadUnstagedDeletedPaths(root);
  const current = new Set([
    ...gitPaths(root, ["ls-files", "-co", "--exclude-standard", "-z"]),
    ...gitPaths(root, ["ls-files", "-o", "-i", "--exclude-standard", "-z"]),
  ]);
  for (const relPath of unstagedDeleted) current.delete(relPath);
  const additions = [...current].filter((relPath) => !isAllowedPath(relPath, planned, known, generatedRules)).sort();
  const missing = [...known].filter((relPath) => !current.has(relPath)
    && !planned.has(relPath)
    && !preexistingDeleted.has(relPath)
    && !generatedRules.some((rule) => rule.test(relPath))).sort();
  const plannedAbsent = [...planned].filter((relPath) => !current.has(relPath)).length;
  const mismatches = await collectContentMismatches(root, current, planned, generatedRules, hashManifest.entries);
  const exitCode = additions.length > 0 ? EXIT_UNKNOWN_ADDITION : missing.length > 0 ? EXIT_MISSING_PATH : mismatches.length > 0 || hashManifest.errors.length > 0 ? EXIT_MISMATCH : 0;
  return { ok: exitCode === 0, exitCode, additions, missing, mismatches, manifestErrors: hashManifest.errors, currentCount: current.size, knownCount: known.size, plannedCount: planned.size, plannedAbsent };
}

export function formatFindings(result) {
  if (result.ok) {
    return `worktree scope OK: ${result.currentCount} current paths, ${result.knownCount} baseline-known, ${result.plannedCount} planned (${result.plannedAbsent} absent)`;
  }
  const lines = [`worktree scope violations (${result.exitCode}):`];
  for (const relPath of result.additions) lines.push(`- [addition] ${relPath}`);
  for (const relPath of result.missing) lines.push(`- [missing] ${relPath}`);
  for (const mismatch of result.mismatches) lines.push(`- [mismatch] ${mismatch.path}`);
  for (const error of result.manifestErrors) lines.push(`- [manifest] ${error}`);
  return lines.join("\n");
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const result = await checkWorktreeScope(args.root, args.baseline, args.planned);
  const text = formatFindings(result);
  if (result.ok) console.log(text);
  else console.error(text);
  return result.exitCode;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isDirectRun) {
  main().then((code) => process.exit(code)).catch((error) => {
    console.error(error.message);
    process.exit(error.exitCode ?? 1);
  });
}
