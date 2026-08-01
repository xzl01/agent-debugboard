import { readFile } from "node:fs/promises";
import path from "node:path";

export const CHECKER_MODULES = Object.freeze([
  "scripts/check-persistent-configuration-docs.mjs",
  "scripts/check-persistent-configuration-docs.test.mjs",
  "scripts/persistent-configuration-docs/contracts.mjs",
  "scripts/persistent-configuration-docs/markdown.mjs",
  "scripts/persistent-configuration-docs/requirements.mjs",
  "scripts/persistent-configuration-docs/validator.mjs",
  "scripts/persistent-configuration-docs/fixtures.mjs",
  "scripts/persistent-configuration-docs/loopback.mjs",
  "scripts/persistent-configuration-docs/source-size.mjs",
  "scripts/persistent-configuration-docs/source-contract-mutations.mjs",
]);

export function pureLoc(source) {
  return source.split(/\r?\n/).filter((line) => {
    const trimmed = line.trim();
    return trimmed !== "" && !trimmed.startsWith("//") && !trimmed.startsWith("/*") && !trimmed.startsWith("*");
  }).length;
}

export async function checkerModuleSizes(root) {
  return Promise.all(CHECKER_MODULES.map(async (relativePath) => ({
    path: relativePath,
    lines: pureLoc(await readFile(path.join(root, relativePath), "utf8")),
  })));
}
