// Static assertion for development-only React diagnostics wiring.
//
// Contract:
// - src/ must gate `import("react-grab")` and `import("react-scan")` behind
//   `import.meta.env.DEV` and the `VITE_DISABLE_REACT_DEVTOOLS` flag.
// - No static (top-level) import of either package may exist anywhere in src/.
// - The production build output must not contain either package name in any
//   emitted .js/.html file.
//
// CLI: node scripts/check-dev-diagnostics.mjs [--src DIR] [--dist DIR]
import { readdir, readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const webRoot = path.resolve(scriptDir, "..");

const REQUIRED_TOKENS = [
  "import.meta.env.DEV",
  "VITE_DISABLE_REACT_DEVTOOLS",
  'import("react-grab")',
  'import("react-scan")',
];

const STATIC_IMPORT_PATTERN =
  /^\s*import\s+(?:[^"']*from\s+)?["']react-(?:grab|scan)["']/m;

const PRODUCTION_FORBIDDEN_PATTERN = /react-grab|react-scan/;

async function collectFiles(dir, extensions) {
  const entries = await readdir(dir, { withFileTypes: true, recursive: true });
  const files = [];
  for (const entry of entries) {
    if (!entry.isFile()) continue;
    const fullPath = path.join(entry.parentPath ?? entry.path, entry.name);
    if (extensions.some((ext) => entry.name.endsWith(ext))) {
      files.push(fullPath);
    }
  }
  return files;
}

export function findSourceViolations(sources) {
  const violations = [];
  const combined = sources.map((source) => source.content).join("\n");
  for (const token of REQUIRED_TOKENS) {
    if (!combined.includes(token)) {
      violations.push(`missing dev-only wiring token: ${token}`);
    }
  }
  for (const source of sources) {
    if (STATIC_IMPORT_PATTERN.test(source.content)) {
      violations.push(`static react diagnostics import in ${source.path}`);
    }
  }
  return violations;
}

export function findProductionViolations(distFiles) {
  const violations = [];
  for (const file of distFiles) {
    if (PRODUCTION_FORBIDDEN_PATTERN.test(file.content)) {
      violations.push(`react diagnostics leaked into production output: ${file.path}`);
    }
  }
  return violations;
}

export async function runCheck({ srcDir, distDir }) {
  const srcPaths = await collectFiles(srcDir, [".ts", ".tsx"]);
  const sources = [];
  for (const srcPath of srcPaths) {
    sources.push({ path: srcPath, content: await readFile(srcPath, "utf8") });
  }
  const distPaths = await collectFiles(distDir, [".js", ".html"]);
  const distFiles = [];
  for (const distPath of distPaths) {
    distFiles.push({ path: distPath, content: await readFile(distPath, "utf8") });
  }
  return [
    ...findSourceViolations(sources),
    ...findProductionViolations(distFiles),
  ];
}

export function parseArgs(argv) {
  const options = {
    srcDir: path.join(webRoot, "src"),
    distDir: path.join(webRoot, "dist"),
  };
  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];
    if (arg === "--src" || arg === "--dist") {
      index += 1;
      if (index >= argv.length) throw new Error(`${arg} requires a value`);
      if (arg === "--src") options.srcDir = path.resolve(argv[index]);
      else options.distDir = path.resolve(argv[index]);
    } else {
      throw new Error(`unknown argument: ${arg}`);
    }
  }
  return options;
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  const violations = await runCheck(options);
  if (violations.length > 0) {
    console.error("dev-diagnostics check FAILED:");
    for (const violation of violations) {
      console.error(`  - ${violation}`);
    }
    process.exitCode = 1;
    return;
  }
  console.log(
    `dev-diagnostics check OK (src=${options.srcDir} dist=${options.distDir})`
  );
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch((error) => {
    console.error(error instanceof Error ? error.message : String(error));
    process.exitCode = 1;
  });
}
