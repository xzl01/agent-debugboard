import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtemp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import os from "node:os";
import test from "node:test";
import { fileURLToPath } from "node:url";
import {
  checkSkillBoundary,
  checkSkillBoundaryContents,
  formatFailures,
} from "./check-skill-boundary.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const TARGET_SKILL = "skills/radxa-linkr-debugger/SKILL.md";
const FRONTMATTER = "---\nname: fixture\ndescription: fixture operation skill\n---\n";

test("the current skill passes the tracked-source boundary contract", async () => {
  const result = await checkSkillBoundary(ROOT);
  assert.equal(result.ok, true, formatFailures(result.failures));
  assert.deepEqual(result.failures, []);
});

test("the current skill keeps the required operation safety tokens", async () => {
  const content = await readFile(path.join(ROOT, "skills/radxa-linkr-debugger/SKILL.md"), "utf8");
  for (const token of [
    "linkr_board_status",
    "next_cursor",
    "automatically replay",
    "requires_confirm",
    "task run",
    "radxa-linkr-debugger-rp2350.uf2",
    "zephyr.uf2",
    "radxa-linkr-debugger-rp2350-ota.bin",
    "linkr-debugger:~$ bootloader",
    "openocd -f interface/<ch347-interface>.cfg",
  ]) {
    assert.ok(content.includes(token), `missing operation token: ${token}`);
  }
});

test("target OpenOCD and target diagnostics are allowed", () => {
  const result = checkSkillBoundaryContents(`${FRONTMATTER}
## Target diagnostics

Power the target and run openocd -f interface/ch347.cfg -f target/board.cfg.
`, { enforceLineCount: false });
  assert.equal(result.ok, true, formatFailures(result.failures));
});

test("self-development and historical markers are rejected", () => {
  const cases = [
    ["source-build", "cargo build --manifest-path cmd-ng/Cargo.toml"],
    ["hil-procedure", "## Web OTA HIL Automation\nRun the board test matrix."],
    ["playwright", "Use Playwright to assert the board-hosted page."],
    ["nightly-release", "gh release upload nightly artifact.bin"],
    ["historical-measurement", "flash 734652/847832 B and RAM 494272/532480 bytes"],
    ["implementation-debugging", "## Firmware implementation diagnostics\nInspect the source internals."],
  ];
  for (const [code, fragment] of cases) {
    const result = checkSkillBoundaryContents(`${FRONTMATTER}\n${fragment}\n`, { enforceLineCount: false });
    assert.equal(result.ok, false, `fixture unexpectedly passed: ${code}`);
    assert.ok(result.failures.some((failure) => failure.code === code), formatFailures(result.failures));
  }
});

test("self-development markers are rejected inside Persistent Configuration too", () => {
  for (const [code, fragment] of [
    ["source-build", "cargo build --manifest-path cmd-ng/Cargo.toml"],
    ["hil-procedure", "sh skills/radxa-linkr-debugger/scripts/config-persistence-hil.sh --dry-run safe-reboot"],
  ]) {
    const result = checkSkillBoundaryContents(`${FRONTMATTER}\n## Persistent Configuration\n${fragment}\n`, { enforceLineCount: false });
    assert.equal(result.ok, false, `fixture unexpectedly passed: ${code}`);
    assert.ok(result.failures.some((failure) => failure.code === code), formatFailures(result.failures));
  }
});

test("a clean checkout does not require an ignored migration map", async () => {
  const root = await mkdtemp(path.join(os.tmpdir(), "skill-boundary-"));
  try {
    const content = await readFile(path.join(ROOT, TARGET_SKILL), "utf8");
    const skillPath = path.join(root, TARGET_SKILL);
    await mkdir(path.dirname(skillPath), { recursive: true });
    await writeFile(skillPath, content);
    const links = new Set([...content.matchAll(/\]\(\.\.\/\.\.\/(docs\/[^)#]+)/g)].map((match) => match[1]));
    for (const link of links) {
      const target = path.join(root, link);
      await mkdir(path.dirname(target), { recursive: true });
      await writeFile(target, "canonical\n");
    }
    const result = await checkSkillBoundary(root);
    assert.equal(result.ok, true, formatFailures(result.failures));
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test("CLI reports the current repository contract as OK", () => {
  const result = execFileSync("node", [
    path.join(ROOT, "scripts/check-skill-boundary.mjs"),
    "--root",
    ROOT,
  ], { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] });
  assert.match(result, /skill-boundary OK/);
});
