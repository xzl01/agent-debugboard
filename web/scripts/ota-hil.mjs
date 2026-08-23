import { access, mkdir, stat } from "node:fs/promises";
import path from "node:path";
import { pathToFileURL, fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(scriptDir, "../..");

export const DEFAULT_BOARD_URL = "http://172.29.203.1";
export const DEFAULT_IMAGE = path.join(
  repoRoot,
  "build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin"
);

const DEFAULT_SCREENSHOT_DIR = "/tmp/opencode";
const DEFAULT_REBOOT_TIMEOUT_MS = 45_000;
const DEFAULT_UPLOAD_TIMEOUT_MS = 120_000;
const DEFAULT_SHORT_TIMEOUT_MS = 5_000;

export function parseOtaHilArgs(argv, env = process.env) {
  const options = {
    boardUrl: DEFAULT_BOARD_URL,
    image: DEFAULT_IMAGE,
    flow: "both",
    dryRun: true,
    chromiumExecutable: env.CHROMIUM_BIN || env.CHROME_BIN || "",
    playwrightModule: env.PLAYWRIGHT_CORE_MODULE || env.PLAYWRIGHT_CORE_PATH || "",
    screenshotDir: env.OTA_HIL_SCREENSHOT_DIR || DEFAULT_SCREENSHOT_DIR,
    headless: env.OTA_HIL_HEADFUL === "1" ? false : true,
    diagnosticsLimit: 12,
    shortTimeoutMs: DEFAULT_SHORT_TIMEOUT_MS,
    rebootTimeoutMs: DEFAULT_REBOOT_TIMEOUT_MS,
    uploadTimeoutMs: DEFAULT_UPLOAD_TIMEOUT_MS,
  };

  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];
    const readValue = () => {
      index += 1;
      if (index >= argv.length) {
        throw new Error(`${arg} requires a value`);
      }
      return argv[index];
    };

    switch (arg) {
      case "--url":
      case "--board-url":
        options.boardUrl = readValue().replace(/\/$/, "");
        break;
      case "--image":
        options.image = path.resolve(readValue());
        break;
      case "--flow":
        options.flow = readValue();
        break;
      case "--execute":
        options.dryRun = false;
        break;
      case "--dry-run":
        options.dryRun = true;
        break;
      case "--chromium-executable":
        options.chromiumExecutable = readValue();
        break;
      case "--playwright-module":
        options.playwrightModule = readValue();
        break;
      case "--screenshot-dir":
        options.screenshotDir = path.resolve(readValue());
        break;
      case "--headful":
        options.headless = false;
        break;
      case "--diagnostics-limit":
        options.diagnosticsLimit = parsePositiveInteger(readValue(), "--diagnostics-limit");
        break;
      case "--short-timeout-ms":
        options.shortTimeoutMs = parsePositiveInteger(readValue(), "--short-timeout-ms");
        break;
      case "--reboot-timeout-ms":
        options.rebootTimeoutMs = parsePositiveInteger(readValue(), "--reboot-timeout-ms");
        break;
      case "--upload-timeout-ms":
        options.uploadTimeoutMs = parsePositiveInteger(readValue(), "--upload-timeout-ms");
        break;
      case "--manual-only":
        options.flow = "manual";
        break;
      case "--auto-only":
        options.flow = "auto";
        break;
      case "--help":
      case "-h":
        options.help = true;
        break;
      default:
        throw new Error(`unknown argument: ${arg}`);
    }
  }

  if (!["auto", "manual", "both"].includes(options.flow)) {
    throw new Error(`--flow must be auto, manual, or both; got ${options.flow}`);
  }

  return options;
}

function parsePositiveInteger(value, label) {
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed <= 0) {
    throw new Error(`${label} must be a positive integer`);
  }
  return parsed;
}

export function usage() {
  return `Usage: node scripts/ota-hil.mjs [options]\n\n` +
    `Browser-driven Web UI OTA HIL runner. Defaults to dry-run.\n\n` +
    `Options:\n` +
    `  --execute                         Run browser actions against real hardware\n` +
    `  --dry-run                         Print bounded plan only (default)\n` +
    `  --url, --board-url URL            Board Web UI URL (default: ${DEFAULT_BOARD_URL})\n` +
    `  --image PATH                      MCUboot .bin OTA payload\n` +
    `  --flow auto|manual|both           Confirm flow coverage (default: both)\n` +
    `  --playwright-module PATH_OR_NAME  Explicit playwright-core module path/name\n` +
    `  --chromium-executable PATH        Explicit Chromium executable path\n` +
    `  --screenshot-dir PATH             Diagnostics output directory\n` +
    `  --headful                         Launch Chromium with headless=false\n`;
}

export function createDryRunPlan(options) {
  const flows = options.flow === "both" ? ["auto", "manual"] : [options.flow];
  return {
    mode: options.dryRun ? "dry-run" : "execute",
    boardUrl: options.boardUrl,
    image: options.image,
    flows,
    browser: {
      playwrightModule: options.playwrightModule || "playwright-core from installed package resolution",
      chromiumExecutable: options.chromiumExecutable || "playwright default executable lookup",
      headless: options.headless,
    },
    safety: [
      "dry-run is default",
      "no Playwright import until execution",
      "image path and .bin extension validated before upload",
      "browser confirms only when --flow manual or both is requested",
      "firmware owns auto-confirm; browser only waits for reported state",
      "watchdog rollback is BLOCKED because no safe fault-injection path exists",
    ],
    steps: flows.flatMap((flow) => [
      `open real Web UI at ${options.boardUrl}/`,
      "expand Debugger maintenance and locate Firmware OTA",
      `select ${options.image} with the page file input`,
      "wait for browser-computed SHA-256 ready state",
      "click Upload image and wait for verified state",
      "accept Start test boot confirmation dialog",
      flow === "auto"
        ? "wait for pending_test, then idle/current_image_confirmed=true without clicking Confirm image"
        : "wait for pending_test, click Confirm image, then wait for idle/current_image_confirmed=true",
    ]),
  };
}

export function summarizeStatus(status) {
  return {
    state: typeof status?.state === "string" ? status.state : "unknown",
    confirmed: typeof status?.current_image_confirmed === "boolean" ? status.current_image_confirmed : null,
  };
}

export function trimDiagnostics(items, limit) {
  if (items.length <= limit) {
    return items;
  }
  return items.slice(items.length - limit);
}

export async function validateExecutionInputs(options) {
  if (!options.image.endsWith(".bin")) {
    throw new Error(`OTA image must be a MCUboot .bin file: ${options.image}`);
  }
  const imageStat = await stat(options.image);
  if (!imageStat.isFile() || imageStat.size <= 0) {
    throw new Error(`OTA image must be a non-empty file: ${options.image}`);
  }
  if (options.chromiumExecutable) {
    await access(options.chromiumExecutable);
  }
}

export async function ensureScreenshotDirectory(screenshotDir) {
  await mkdir(screenshotDir, { recursive: true });
}

export async function loadPlaywrightCore(moduleSpecifier) {
  const candidates = [];
  if (moduleSpecifier) {
    candidates.push(moduleSpecifier);
  }
  candidates.push("playwright-core");

  const errors = [];
  for (const candidate of candidates) {
    try {
      const specifier = candidate.startsWith("/") || candidate.startsWith(".")
        ? pathToFileURL(path.resolve(candidate)).href
        : candidate;
      return await import(specifier);
    } catch (error) {
      errors.push(`${candidate}: ${error instanceof Error ? error.message : String(error)}`);
    }
  }
  throw new Error(`Unable to load playwright-core. Set --playwright-module or PLAYWRIGHT_CORE_MODULE. ${errors.join(" | ")}`);
}

async function fetchOtaStatus(boardUrl, timeoutMs) {
  const response = await fetch(`${boardUrl}/api/v1/ota`, { signal: AbortSignal.timeout(timeoutMs) });
  if (!response.ok) {
    throw new Error(`OTA status HTTP ${response.status}`);
  }
  return await response.json();
}

async function waitForOtaState(boardUrl, predicate, timeoutMs, diagnostics) {
  const deadline = Date.now() + timeoutMs;
  let lastStatus = null;
  while (Date.now() < deadline) {
    try {
      lastStatus = await fetchOtaStatus(boardUrl, DEFAULT_SHORT_TIMEOUT_MS);
      diagnostics.push({ t: Date.now(), ...summarizeStatus(lastStatus) });
      if (predicate(lastStatus)) {
        return lastStatus;
      }
    } catch (error) {
      diagnostics.push({
        t: Date.now(),
        state: "unreachable",
        error: error instanceof Error ? error.message : String(error),
      });
    }
    await new Promise((resolve) => setTimeout(resolve, 750));
  }
  throw new Error(`Timed out waiting for OTA state; last=${JSON.stringify(lastStatus)}`);
}

export function locateOtaCard(page) {
  return page.locator("section", {
    has: page.getByRole("heading", { name: "Firmware OTA", exact: true }),
  });
}

export async function openOtaCard(page, boardUrl) {
  await page.goto(`${boardUrl}/`, { waitUntil: "networkidle", timeout: 30_000 });
  await page.getByText("Debugger maintenance", { exact: true }).click();
  await page.getByText("Firmware OTA", { exact: true }).waitFor({ timeout: 10_000 });
}

export async function selectAndUpload(otaCard, image, uploadTimeoutMs) {
  await otaCard.locator('input[type="file"][accept*=".bin"]').setInputFiles(image);
  await otaCard.getByText("SHA-256 computed locally. Ready to upload.", { exact: true }).waitFor({ timeout: 30_000 });
  const upload = otaCard.getByRole("button", { name: "Upload image" });
  if (!(await upload.isEnabled())) {
    throw new Error("Upload image is disabled after hashing");
  }
  await upload.click();
  await otaCard.getByText("verified", { exact: true }).waitFor({ timeout: uploadTimeoutMs });
}

export async function startTestBoot(page, otaCard) {
  page.once("dialog", (dialog) => dialog.accept());
  await otaCard.getByRole("button", { name: "Start test boot" }).click();
}

export async function waitForOtaCardConfirmed(otaCard, timeoutMs) {
  await otaCard.getByText("idle", { exact: true }).waitFor({ timeout: timeoutMs });
  await otaCard.getByText("confirmed", { exact: true }).waitFor({ timeout: timeoutMs });
}

async function runConfirmFlow(page, otaCard, boardUrl, options, flow, diagnostics) {
  await selectAndUpload(otaCard, options.image, options.uploadTimeoutMs);
  await page.screenshot({ path: path.join(options.screenshotDir, `ota-hil-${flow}-verified.png`), fullPage: true });
  await startTestBoot(page, otaCard);
  await waitForOtaState(boardUrl, (status) => status.state === "pending_test", options.rebootTimeoutMs, diagnostics);

  if (flow === "manual") {
    const confirm = otaCard.getByRole("button", { name: "Confirm image" });
    await confirm.waitFor({ state: "visible", timeout: 10_000 });
    const deadline = Date.now() + 10_000;
    while (!(await confirm.isEnabled()) && Date.now() < deadline) {
      await new Promise((resolve) => setTimeout(resolve, 250));
    }
    if (!(await confirm.isEnabled())) {
      throw new Error("Confirm image did not enable in pending_test");
    }
    await confirm.click();
  }

  const confirmed = await waitForOtaState(
    boardUrl,
    (status) => status.state === "idle" && status.current_image_confirmed === true,
    flow === "auto" ? 45_000 : 15_000,
    diagnostics
  );
  await waitForOtaCardConfirmed(otaCard, options.rebootTimeoutMs);
  await page.screenshot({ path: path.join(options.screenshotDir, `ota-hil-${flow}-confirmed.png`), fullPage: true });
  return confirmed;
}

export async function runBrowserOtaHil(options) {
  await validateExecutionInputs(options);
  await ensureScreenshotDirectory(options.screenshotDir);
  const { chromium } = await loadPlaywrightCore(options.playwrightModule);
  const launchOptions = { headless: options.headless };
  if (options.chromiumExecutable) {
    launchOptions.executablePath = options.chromiumExecutable;
  }

  const browser = await chromium.launch(launchOptions);
  let context;
  try {
    const created = await createHilBrowserPage(browser);
    context = created.context;
    const page = created.page;
    const consoleErrors = [];
    page.on("console", (message) => {
      if (message.type() === "error") {
        consoleErrors.push(message.text());
      }
    });

    await openOtaCard(page, options.boardUrl);
    const otaCard = locateOtaCard(page);
    const initial = await fetchOtaStatus(options.boardUrl, options.shortTimeoutMs);
    const flows = options.flow === "both" ? ["auto", "manual"] : [options.flow];
    const results = [];
    const diagnostics = [];

    for (const flow of flows) {
      const confirmed = await runConfirmFlow(page, otaCard, options.boardUrl, options, flow, diagnostics);
      results.push({ flow, confirmed: summarizeStatus(confirmed) });
    }

    const unexpectedConsoleErrors = consoleErrors.filter(
      (message) => !message.includes("Failed to load resource") && !message.includes("NetworkError")
    );
    if (unexpectedConsoleErrors.length > 0) {
      throw new Error(`Unexpected console errors: ${trimDiagnostics(unexpectedConsoleErrors, options.diagnosticsLimit).join(" | ")}`);
    }

    return {
      passed: true,
      initial: summarizeStatus(initial),
      results,
      diagnostics: trimDiagnostics(diagnostics, options.diagnosticsLimit),
      watchdogRollback: "BLOCKED: no safe fault-injection path is available; not attempted",
    };
  } finally {
    if (context) {
      await context.close();
    }
    await browser.close();
  }
}

export async function createHilBrowserPage(browser) {
  const context = await browser.newContext({ viewport: { width: 1440, height: 1000 } });
  await context.addInitScript(() => localStorage.setItem("lang", "en"));
  const page = await context.newPage();
  return { context, page };
}

async function main() {
  const options = parseOtaHilArgs(process.argv.slice(2));
  if (options.help) {
    console.log(usage());
    return;
  }

  if (options.dryRun) {
    console.log(JSON.stringify(createDryRunPlan(options), null, 2));
    return;
  }

  const result = await runBrowserOtaHil(options);
  console.log(JSON.stringify(result, null, 2));
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch((error) => {
    console.error(error instanceof Error ? error.message : String(error));
    process.exitCode = 1;
  });
}
