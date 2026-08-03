import { readdir, readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { discoverTests } from "../web/scripts/run-tests.mjs";

const FIRMWARE_TEST_DIR = "apps/radxa_linkr_debugger/tests";
const FIRMWARE_RUNNER = `${FIRMWARE_TEST_DIR}/run_unit_tests.sh`;

async function files(directory, pattern) {
  let entries;
  try {
    entries = await readdir(directory, { withFileTypes: true });
  } catch (error) {
    if (error?.code === "ENOENT") return [];
    throw error;
  }
  return entries.filter((entry) => entry.isFile() && pattern.test(entry.name)).map((entry) => entry.name).sort();
}

export async function checkTestRegistration(repositoryRoot) {
  const root = path.resolve(repositoryRoot);
  const failures = [];
  const runnerPath = path.join(root, FIRMWARE_RUNNER);
  let runner = "";
  try {
    runner = await readFile(runnerPath, "utf8");
  } catch (error) {
    failures.push(`${FIRMWARE_RUNNER}: ${error?.code === "ENOENT" ? "missing test runner" : String(error)}`);
  }
  const registeredRunnerLines = runner
    .split(/\r?\n/)
    .filter((line) => !line.trimStart().startsWith("#"))
    .join("\n");

  const cTests = await files(path.join(root, FIRMWARE_TEST_DIR, "model_host"), /^test_.*\.c$/);
  for (const name of cTests) {
    if (!registeredRunnerLines.includes(name)) failures.push(`${FIRMWARE_TEST_DIR}/model_host/${name}: not registered in run_unit_tests.sh`);
  }

  const pythonDir = path.join(root, FIRMWARE_TEST_DIR);
  const pythonTests = await files(pythonDir, /^test_.*\.py$/);
  for (const name of pythonTests) {
    const source = await readFile(path.join(pythonDir, name), "utf8");
    const isOfflineUnitTest = /unittest\.(?:TestCase|main)\b/.test(source);
    if (isOfflineUnitTest && !registeredRunnerLines.includes(name)) {
      failures.push(`${FIRMWARE_TEST_DIR}/${name}: offline unit test is not registered in run_unit_tests.sh`);
    }
  }

  const webTests = await discoverTests(path.join(root, "web"));
  for (const name of webTests.unclassified) failures.push(`web/${name}: test runner is not declared`);
  if (webTests.node.length + webTests.browser.length === 0) failures.push("web: no tests discovered");

  return { ok: failures.length === 0, failures };
}

export function formatFailures(failures) {
  return ["test registration check failed:", ...failures.map((failure) => `- ${failure}`)].join("\n");
}

async function main() {
  const args = process.argv.slice(2);
  if (args.length !== 2 || args[0] !== "--root") {
    console.error("usage: node scripts/check-test-registration.mjs --root <repository-root>");
    process.exitCode = 2;
    return;
  }
  const result = await checkTestRegistration(args[1]);
  if (!result.ok) {
    console.error(formatFailures(result.failures));
    process.exitCode = 1;
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) await main();
