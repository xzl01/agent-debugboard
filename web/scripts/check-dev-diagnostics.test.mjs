import assert from "node:assert/strict";
import { mkdtemp, mkdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { test } from "node:test";
import {
  findProductionViolations,
  findSourceViolations,
  runCheck,
} from "./check-dev-diagnostics.mjs";

const WIRED_SOURCE = [
  'if (shouldEnable(import.meta.env.DEV, import.meta.env.VITE_DISABLE_REACT_DEVTOOLS)) {',
  '  await import("react-grab");',
  '  const { scan } = await import("react-scan");',
  "  scan({ enabled: true });",
  "}",
].join("\n");

test("accepts fully gated dynamic wiring", () => {
  const violations = findSourceViolations([
    { path: "main.tsx", content: WIRED_SOURCE },
  ]);
  assert.deepEqual(violations, []);
});

test("fails when no gating exists", () => {
  const violations = findSourceViolations([
    { path: "main.tsx", content: 'import App from "./App";' },
  ]);
  assert.equal(violations.length, 4);
});

test("rejects a static react diagnostics import", () => {
  const violations = findSourceViolations([
    { path: "main.tsx", content: `${WIRED_SOURCE}\nimport "react-scan";\n` },
  ]);
  assert.ok(
    violations.some((violation) => violation.includes("static react diagnostics import"))
  );
});

test("accepts a clean production bundle", () => {
  const violations = findProductionViolations([
    { path: "dist/assets/app.js", content: "console.log('dashboard');" },
  ]);
  assert.deepEqual(violations, []);
});

test("fails when a diagnostics package leaks into production output", () => {
  const violations = findProductionViolations([
    { path: "dist/assets/app.js", content: "/* react-scan toolbar */" },
  ]);
  assert.equal(violations.length, 1);
  assert.ok(violations[0].includes("dist/assets/app.js"));
});

test("runCheck reports violations from a seeded fixture tree", async (t) => {
  const root = await mkdtemp(path.join(tmpdir(), "dev-diag-check-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const srcDir = path.join(root, "src");
  const distDir = path.join(root, "dist");
  await mkdir(srcDir, { recursive: true });
  await mkdir(distDir, { recursive: true });
  await writeFile(path.join(srcDir, "main.tsx"), 'import App from "./App";\n');
  await writeFile(
    path.join(distDir, "app.js"),
    "const marker = 'react-grab';\n"
  );
  const violations = await runCheck({ srcDir, distDir });
  assert.ok(violations.some((violation) => violation.includes("missing dev-only wiring token")));
  assert.ok(violations.some((violation) => violation.includes("leaked into production output")));
});
