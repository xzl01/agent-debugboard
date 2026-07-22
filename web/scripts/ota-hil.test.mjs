import test from "node:test";
import assert from "node:assert/strict";
import { mkdtemp, rm, stat } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import {
  DEFAULT_BOARD_URL,
  createHilBrowserPage,
  createDryRunPlan,
  ensureScreenshotDirectory,
  parseOtaHilArgs,
  summarizeStatus,
  trimDiagnostics,
} from "./ota-hil.mjs";

test("parses browser OTA HIL arguments without loading Playwright", () => {
  const options = parseOtaHilArgs([
    "--execute",
    "--flow",
    "manual",
    "--url",
    "http://172.29.203.1/",
    "--image",
    "../firmware.bin",
    "--chromium-executable",
    "/opt/chromium/chrome",
    "--playwright-module",
    "/opt/playwright-core/index.mjs",
  ], {});

  assert.equal(options.dryRun, false);
  assert.equal(options.flow, "manual");
  assert.equal(options.boardUrl, DEFAULT_BOARD_URL);
  assert.equal(options.chromiumExecutable, "/opt/chromium/chrome");
  assert.equal(options.playwrightModule, "/opt/playwright-core/index.mjs");
  assert.match(options.image, /firmware\.bin$/);
});

test("defaults to dry-run and plans both browser confirmation flows", () => {
  const options = parseOtaHilArgs([], {});
  const plan = createDryRunPlan(options);

  assert.equal(plan.mode, "dry-run");
  assert.deepEqual(plan.flows, ["auto", "manual"]);
  assert.equal(plan.safety.some((item) => item.includes("watchdog rollback is BLOCKED")), true);
  assert.equal(plan.steps.some((step) => step.includes("without clicking Confirm image")), true);
  assert.equal(plan.steps.some((step) => step.includes("click Confirm image")), true);
});

test("rejects unknown or invalid flow arguments", () => {
  assert.throws(() => parseOtaHilArgs(["--flow", "rollback"], {}), /--flow must be/);
  assert.throws(() => parseOtaHilArgs(["--unknown"], {}), /unknown argument/);
  assert.throws(() => parseOtaHilArgs(["--diagnostics-limit", "0"], {}), /positive integer/);
});

test("normalizes bounded status diagnostics", () => {
  assert.deepEqual(summarizeStatus({ state: "idle", current_image_confirmed: true }), {
    state: "idle",
    confirmed: true,
  });
  assert.deepEqual(summarizeStatus({}), { state: "unknown", confirmed: null });
  assert.deepEqual(trimDiagnostics([1, 2, 3, 4], 2), [3, 4]);
});

test("creates screenshot directory recursively without Playwright", async () => {
  const root = await mkdtemp(path.join(os.tmpdir(), "linkr-ota-hil-"));
  const nested = path.join(root, "screens", "ota");
  try {
    await ensureScreenshotDirectory(nested);
    assert.equal((await stat(nested)).isDirectory(), true);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test("creates Playwright pages through a browser context", async () => {
  const calls = [];
  const page = {};
  const context = {
    async addInitScript(callback) {
      calls.push(["addInitScript", typeof callback]);
    },
    async newPage() {
      calls.push(["context.newPage"]);
      return page;
    },
  };
  const browser = {
    async newContext(options) {
      calls.push(["browser.newContext", options]);
      return context;
    },
    async newPage() {
      throw new Error("browser.newPage must not be used");
    },
  };

  const created = await createHilBrowserPage(browser);

  assert.equal(created.context, context);
  assert.equal(created.page, page);
  assert.deepEqual(calls, [
    ["browser.newContext", { viewport: { width: 1440, height: 1000 } }],
    ["addInitScript", "function"],
    ["context.newPage"],
  ]);
});
