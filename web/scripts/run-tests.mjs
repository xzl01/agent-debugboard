import { readdir, readFile } from "node:fs/promises";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const DEFAULT_ROOT = path.resolve(SCRIPT_DIR, "..");
const TEST_FILE = /\.test\.(?:[cm]?js|tsx?)$/;

function skipTrivia(source, start) {
  let index = start;
  while (index < source.length) {
    if (/\s/.test(source[index])) {
      index += 1;
    } else if (source.startsWith("//", index)) {
      index = source.indexOf("\n", index + 2);
      if (index < 0) return source.length;
    } else if (source.startsWith("/*", index)) {
      const end = source.indexOf("*/", index + 2);
      index = end < 0 ? source.length : end + 2;
    } else {
      break;
    }
  }
  return index;
}

function readLiteral(source, start) {
  const quote = source[start];
  let value = "";
  let index = start + 1;
  while (index < source.length) {
    if (source[index] === "\\") {
      if (quote !== "`") value += source[index + 1] ?? "";
      index += 2;
    } else if (source[index] === quote) {
      return { end: index + 1, value };
    } else {
      value += source[index];
      index += 1;
    }
  }
  return { end: source.length, value };
}

function readIdentifier(source, start) {
  const match = /^[A-Za-z_$][\w$]*/.exec(source.slice(start));
  return match ? { end: start + match[0].length, value: match[0] } : null;
}

function previousNonSpace(source, start) {
  let index = start - 1;
  while (index >= 0 && /\s/.test(source[index])) index -= 1;
  return source[index] ?? "";
}

function importedModules(source) {
  const modules = new Set();
  let braceDepth = 0;
  let index = 0;

  while (index < source.length) {
    index = skipTrivia(source, index);
    const char = source[index];
    if (!char) break;
    if (char === "'" || char === '"' || char === "`") {
      index = readLiteral(source, index).end;
      continue;
    }
    if (char === "{") {
      braceDepth += 1;
      index += 1;
      continue;
    }
    if (char === "}") {
      braceDepth = Math.max(0, braceDepth - 1);
      index += 1;
      continue;
    }

    const identifier = readIdentifier(source, index);
    if (!identifier) {
      index += 1;
      continue;
    }
    const previous = previousNonSpace(source, index);
    index = identifier.end;
    if (braceDepth !== 0 || previous === ".") continue;

    if (identifier.value === "require") {
      let cursor = skipTrivia(source, index);
      if (source[cursor] !== "(") continue;
      cursor = skipTrivia(source, cursor + 1);
      if (source[cursor] !== "'" && source[cursor] !== '"') continue;
      modules.add(readLiteral(source, cursor).value);
      continue;
    }
    if (identifier.value !== "import") continue;

    let cursor = skipTrivia(source, index);
    if (source[cursor] === "(" || source[cursor] === ".") continue;
    if (source[cursor] === "'" || source[cursor] === '"') {
      modules.add(readLiteral(source, cursor).value);
      continue;
    }
    while (cursor < source.length && source[cursor] !== ";") {
      cursor = skipTrivia(source, cursor);
      const token = readIdentifier(source, cursor);
      if (!token) {
        cursor += 1;
        continue;
      }
      cursor = token.end;
      if (token.value !== "from") continue;
      cursor = skipTrivia(source, cursor);
      if (source[cursor] === "'" || source[cursor] === '"') {
        modules.add(readLiteral(source, cursor).value);
      }
      break;
    }
  }

  return modules;
}

async function walk(directory) {
  let entries;
  try {
    entries = await readdir(directory, { withFileTypes: true });
  } catch (error) {
    if (error?.code === "ENOENT") return [];
    throw error;
  }

  const files = [];
  for (const entry of entries) {
    if (entry.name === "node_modules" || entry.name === "dist") continue;
    const absolute = path.join(directory, entry.name);
    if (entry.isDirectory()) files.push(...await walk(absolute));
    else if (entry.isFile() && TEST_FILE.test(entry.name)) files.push(absolute);
  }
  return files;
}

export async function discoverTests(root = DEFAULT_ROOT) {
  const candidates = [
    ...await walk(path.join(root, "src")),
    ...await walk(path.join(root, "scripts")),
  ].sort();
  const node = [];
  const browser = [];
  const unclassified = [];

  for (const absolute of candidates) {
    const source = await readFile(absolute, "utf8");
    const relative = path.relative(root, absolute).split(path.sep).join("/");
    const modules = importedModules(source);
    if (modules.has("vitest") && !modules.has("node:test")) browser.push(relative);
    else if (modules.has("node:test") && !modules.has("vitest")) node.push(relative);
    else unclassified.push(relative);
  }

  return { node, browser, unclassified };
}

function run(command, args, cwd) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { cwd, env: process.env, stdio: "inherit" });
    child.once("error", reject);
    child.once("exit", (code, signal) => {
      if (code === 0) resolve();
      else reject(new Error(`${command} exited with ${signal ?? code}`));
    });
  });
}

export async function runDiscoveredTests(root = DEFAULT_ROOT) {
  const tests = await discoverTests(root);
  if (tests.unclassified.length > 0) {
    throw new Error(`test files must import node:test or vitest:\n${tests.unclassified.join("\n")}`);
  }
  if (tests.node.length === 0 && tests.browser.length === 0) {
    throw new Error("no test files discovered");
  }

  if (tests.node.length > 0) {
    await run(process.execPath, ["--experimental-strip-types", "--test", ...tests.node], root);
  }
  if (tests.browser.length > 0) {
    const vitest = path.join(root, "node_modules", "vitest", "vitest.mjs");
    await run(process.execPath, [vitest, "run", "--environment", "jsdom", ...tests.browser], root);
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  await runDiscoveredTests();
}
