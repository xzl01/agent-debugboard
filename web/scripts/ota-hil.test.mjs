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
  locateOtaCard,
  openOtaCard,
  parseOtaHilArgs,
  selectAndUpload,
  startTestBoot,
  summarizeStatus,
  trimDiagnostics,
  waitForOtaCardConfirmed,
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
  assert.equal(
    plan.safety.some((item) => item.includes("watchdog rollback fault injection is exercised by the shell runner")),
    true
  );
  assert.equal(plan.steps.some((step) => step.includes("without clicking Confirm image")), true);
  assert.equal(plan.steps.some((step) => step.includes("click Confirm image")), true);
  assert.equal(plan.steps.some((step) => step.includes("Debugger maintenance")), true);
  assert.equal(plan.steps.some((step) => step.includes("Advanced & recovery")), false);
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

test("opens the OTA card via the current Debugger maintenance accordion", async () => {
  const calls = [];
  const textQueries = [];
  const page = {
    async goto(url, options) {
      calls.push(["goto", url, options]);
    },
    getByText(text, options) {
      textQueries.push([text, options]);
      return {
        async click() {
          calls.push(["click", text]);
        },
        async waitFor(waitOptions) {
          calls.push(["waitFor", text, waitOptions]);
        },
      };
    },
  };

  await openOtaCard(page, "http://172.29.203.1");

  assert.deepEqual(calls, [
    ["goto", "http://172.29.203.1/", { waitUntil: "networkidle", timeout: 30_000 }],
    ["click", "Debugger maintenance"],
    ["waitFor", "Firmware OTA", { timeout: 10_000 }],
  ]);
  assert.deepEqual(textQueries, [
    ["Debugger maintenance", { exact: true }],
    ["Firmware OTA", { exact: true }],
  ]);
  assert.equal(
    textQueries.some(([text]) => text.includes("Advanced & recovery")),
    false
  );
});

function createOtaScopeFakes() {
  const pageCalls = [];
  const cardCalls = [];
  const section = {
    locator(selector) {
      cardCalls.push(["locator", selector]);
      return {
        async setInputFiles(file) {
          cardCalls.push(["setInputFiles", file]);
        },
      };
    },
    getByText(text, options) {
      cardCalls.push(["getByText", text, options]);
      return {
        async waitFor(waitOptions) {
          cardCalls.push(["waitFor", text, waitOptions]);
        },
      };
    },
    getByRole(role, options) {
      cardCalls.push(["getByRole", role, options]);
      return {
        async isEnabled() {
          return true;
        },
        async click() {
          cardCalls.push(["click", options.name]);
        },
        async waitFor(waitOptions) {
          cardCalls.push(["waitFor", options.name, waitOptions]);
        },
      };
    },
  };
  const headingProbe = { heading: true };
  const page = {
    getByRole(role, options) {
      pageCalls.push(["getByRole", role, options]);
      return headingProbe;
    },
    locator(selector, options) {
      pageCalls.push(["locator", selector, options]);
      return section;
    },
    getByText() {
      throw new Error("global getByText must not be used for OTA controls");
    },
    once(event, handler) {
      pageCalls.push(["once", event, typeof handler]);
    },
  };
  return { page, pageCalls, cardCalls, headingProbe, section };
}

test("locates the OTA card as the section containing the exact Firmware OTA heading", () => {
  const { page, pageCalls, headingProbe, section } = createOtaScopeFakes();

  const otaCard = locateOtaCard(page);

  assert.equal(otaCard, section);
  assert.deepEqual(pageCalls, [
    ["getByRole", "heading", { name: "Firmware OTA", exact: true }],
    ["locator", "section", { has: headingProbe }],
  ]);
});

test("scopes file input, hash-ready message, upload button, and verified badge to the OTA card", async () => {
  const { page, cardCalls } = createOtaScopeFakes();
  const otaCard = locateOtaCard(page);

  await selectAndUpload(otaCard, "/tmp/firmware.bin", 120_000);

  assert.deepEqual(cardCalls, [
    ["locator", 'input[type="file"][accept*=".bin"]'],
    ["setInputFiles", "/tmp/firmware.bin"],
    ["getByText", "SHA-256 computed locally. Ready to upload.", { exact: true }],
    ["waitFor", "SHA-256 computed locally. Ready to upload.", { timeout: 30_000 }],
    ["getByRole", "button", { name: "Upload image" }],
    ["click", "Upload image"],
    ["getByText", "verified", { exact: true }],
    ["waitFor", "verified", { timeout: 120_000 }],
  ]);
});

test("scopes Start test boot to the OTA card while registering the dialog handler on the page", async () => {
  const { page, pageCalls, cardCalls } = createOtaScopeFakes();
  const otaCard = locateOtaCard(page);
  pageCalls.length = 0;

  await startTestBoot(page, otaCard);

  assert.deepEqual(pageCalls, [["once", "dialog", "function"]]);
  assert.deepEqual(cardCalls, [
    ["getByRole", "button", { name: "Start test boot" }],
    ["click", "Start test boot"],
  ]);
});

test("waits for the scoped OTA card to visibly reach idle and confirmed", async () => {
  const calls = [];
  const otaCard = {
    getByText(text, options) {
      calls.push(["getByText", text, options]);
      return {
        async waitFor(waitOptions) {
          calls.push(["waitFor", text, waitOptions]);
        },
      };
    },
  };

  await waitForOtaCardConfirmed(otaCard, 45_000);

  assert.deepEqual(calls, [
    ["getByText", "idle", { exact: true }],
    ["waitFor", "idle", { timeout: 45_000 }],
    ["getByText", "confirmed", { exact: true }],
    ["waitFor", "confirmed", { timeout: 45_000 }],
  ]);
});

test("rejects while the OTA card still shows a stale pending or unconfirmed state", async () => {
  const rendered = new Set(["pending test", "unconfirmed"]);
  const otaCard = {
    getByText(text) {
      return {
        async waitFor(waitOptions) {
          if (!rendered.has(text)) {
            throw new Error(`Timeout ${waitOptions.timeout}ms exceeded waiting for "${text}"`);
          }
        },
      };
    },
  };

  await assert.rejects(
    () => waitForOtaCardConfirmed(otaCard, 45_000),
    /waiting for "idle"/
  );

  rendered.delete("pending test");
  rendered.add("idle");
  await assert.rejects(
    () => waitForOtaCardConfirmed(otaCard, 45_000),
    /waiting for "confirmed"/
  );
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
