import { readFile, readdir, stat } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const EXIT_USAGE = 2;
const EXIT_VIOLATIONS = 1;
const SKILL_ROOT = "skills";
const TARGET_SKILL = "skills/radxa-linkr-debugger/SKILL.md";
const REQUIRED_OPERATION_TOKENS = [
  "linkr_board_status",
  "curl",
  "radxa-linkr-debuggerctl",
  "CDC ACM",
  "next_cursor",
  "confirm",
  "requires_confirm",
  "radxa-linkr-debugger-rp2350.uf2",
  "radxa-linkr-debugger-rp2350-ota.bin",
  "openocd",
];

const FORBIDDEN_MARKERS = Object.freeze([
  {
    code: "source-build",
    pattern: /\b(?:cargo\s+(?:build|run|test|clippy|fmt)|west\s+(?:build|update|init)|make\s+firmware|npm\s+run\s+build)\b|scripts\/(?:build-firmware|setup-zephyr|config-persistence-hil)\.sh/i,
    detail: "Linkr source-build or repository validation procedure",
  },
  {
    code: "hil-procedure",
    pattern: /\b(?:HIL|hardware-in-the-loop|hil-functional-test-spec|web-ota-hil|ota-hil)\b/i,
    detail: "Linkr board-test or HIL procedure",
  },
  {
    code: "playwright",
    pattern: /\b(?:Playwright|playwright|browser_run_code|page\.goto)\b/i,
    detail: "Playwright or browser self-debugging procedure",
  },
  {
    code: "nightly-release",
    pattern: /\b(?:Rolling Nightly|nightly\s+pre-release|gh\s+release|release\s+workflow|release\s+engineering)\b/i,
    detail: "nightly or release-engineering procedure",
  },
  {
    code: "historical-measurement",
    pattern: /\b(?:historical|measured\s+(?:build|result)|benchmark|flash\s+\d+\/\d+\s+B|RAM\s+\d+\/\d+\s+bytes|SHA256\s+[0-9a-f]{64})\b/i,
    detail: "historical measurement or build-size evidence",
  },
  {
    code: "implementation-debugging",
    pattern: /(?:^|\n)\s*#{1,6}\s+[^\n]*(?:implementation|source[- ]level|internal diagnostics|self-debug)/i,
    detail: "Linkr implementation or self-debugging narrative",
  },
]);

function failure(code, detail, surface = TARGET_SKILL, line = null) {
  return { code, detail, surface, ...(line === null ? {} : { line }) };
}

function frontmatterEnd(content) {
  if (!content.startsWith("---\n")) return -1;
  const delimiter = "\n---\n";
  const end = content.indexOf(delimiter, 4);
  return end < 0 ? -1 : end + delimiter.length;
}

function hasFrontmatterField(frontmatter, name) {
  return new RegExp(`^${name}:\\s*\\S`, "m").test(frontmatter);
}

function scanLines(content, surface) {
  return content.split(/\r?\n/).map((text, index) => ({ text, line: index + 1, surface }));
}

export function checkSkillBoundaryContents(content, options = {}) {
  const {
    enforceLineCount = true,
    surface = TARGET_SKILL,
    requiredTokens = [],
  } = options;
  const failures = [];
  const end = frontmatterEnd(content);
  if (end < 0) {
    failures.push(failure("frontmatter", "frontmatter must start and end with ---", surface));
  } else {
    const frontmatter = content.slice(4, end - 5);
    for (const field of ["name", "description"]) {
      if (!hasFrontmatterField(frontmatter, field)) failures.push(failure("frontmatter", `missing frontmatter field: ${field}`, surface));
    }
  }

  const lineCount = content.split(/\r?\n/).length - (content.endsWith("\n") ? 1 : 0);
  if (enforceLineCount && (lineCount < 400 || lineCount > 700)) {
    failures.push(failure("line-count", `skill must contain 400-700 lines, found ${lineCount}`, surface));
  }

  for (const token of requiredTokens) {
    if (!content.includes(token)) failures.push(failure("operation-token", `missing operation token: ${token}`, surface));
  }

  const body = end < 0 ? content : content.slice(end);
  const lineOffset = end < 0 ? 0 : content.slice(0, end).split(/\r?\n/).length - 1;
  for (const { text, line } of scanLines(body, surface)) {
    for (const marker of FORBIDDEN_MARKERS) {
      if (marker.pattern.test(text)) failures.push(failure(marker.code, marker.detail, surface, line + lineOffset));
    }
  }
  return { ok: failures.length === 0, failures, lineCount };
}

async function collectSkillFiles(root) {
  const found = [];
  async function visit(relativeDirectory) {
    const absoluteDirectory = path.join(root, relativeDirectory);
    for (const entry of await readdir(absoluteDirectory, { withFileTypes: true })) {
      const relativePath = path.join(relativeDirectory, entry.name).replaceAll(path.sep, "/");
      if (entry.isDirectory()) await visit(relativePath);
      else if (entry.name === "SKILL.md") found.push(relativePath);
    }
  }
  await visit(SKILL_ROOT);
  return found.sort();
}

function canonicalLinks(content) {
  const body = content.slice(Math.max(0, frontmatterEnd(content)));
  return [...body.matchAll(/\]\((\.\.\/\.\.\/docs\/[^)#]+)(?:#[^)]+)?\)/g)].map((match) => match[1]);
}

async function checkCanonicalLinks(root, content) {
  const failures = [];
  for (const link of canonicalLinks(content)) {
    const relative = link.replace(/^\.\.\/\.\.\//, "");
    try {
      const entry = await stat(path.join(root, relative));
      if (!entry.isFile()) failures.push(failure("missing-link", `canonical link is not a regular file: ${link}`));
    } catch {
      failures.push(failure("missing-link", `canonical link does not exist: ${link}`));
    }
  }
  return failures;
}

export async function checkSkillBoundary(repositoryRoot) {
  const root = path.resolve(repositoryRoot);
  const failures = [];
  const skillFiles = await collectSkillFiles(root);
  for (const relativePath of skillFiles) {
    const content = await readFile(path.join(root, relativePath), "utf8");
    const result = checkSkillBoundaryContents(content, {
      surface: relativePath,
      enforceLineCount: relativePath === TARGET_SKILL,
      requiredTokens: relativePath === TARGET_SKILL ? REQUIRED_OPERATION_TOKENS : [],
    });
    failures.push(...result.failures);
    if (relativePath === TARGET_SKILL) failures.push(...await checkCanonicalLinks(root, content));
  }
  if (!skillFiles.includes(TARGET_SKILL)) failures.push(failure("skill-missing", `missing ${TARGET_SKILL}`));
  return { ok: failures.length === 0, failures, scanned: skillFiles.length };
}

export function formatFailures(failures) {
  return ["skill-boundary violations:", ...failures.map(({ code, surface, line, detail }) => `- [${code}] ${surface}${line ? `:${line}` : ""}: ${detail}`)].join("\n");
}

function usage() {
  return "usage: node scripts/check-skill-boundary.mjs --root <repository-root>";
}

async function main() {
  const argv = process.argv.slice(2);
  if (argv.length !== 2 || argv[0] !== "--root") {
    console.error(usage());
    return EXIT_USAGE;
  }
  const result = await checkSkillBoundary(argv[1]);
  if (result.ok) console.log(`skill-boundary OK: ${result.scanned} skill file(s) scanned`);
  else console.error(formatFailures(result.failures));
  return result.ok ? 0 : EXIT_VIOLATIONS;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().then((code) => { process.exitCode = code; });
}
