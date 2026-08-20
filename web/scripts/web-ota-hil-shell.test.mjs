import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

const execFileAsync = promisify(execFile);
const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const runner = path.join(repoRoot, "skills/radxa-linkr-debugger/scripts/web-ota-hil.sh");

async function runManualConfirmFixture({ terminalState = null }) {
  let confirmed = terminalState !== null;
  let markerPresent = terminalState === "rollback";
  let confirmRequests = 0;
  const server = http.createServer((request, response) => {
    request.resume();
    response.setHeader("Content-Type", "application/json");
    if (request.method === "POST" && request.url === "/api/v1/ota/upload") {
      response.end('{"ok":true,"state":"verified"}\n');
      return;
    }
    if (request.method === "POST" && request.url === "/api/v1/ota/test") {
      response.end('{"ok":true,"state":"rebooting"}\n');
      return;
    }
    if (request.method === "POST" && request.url === "/api/v1/ota/confirm") {
      confirmRequests += 1;
      confirmed = true;
      markerPresent = false;
      response.end('{"ok":true,"state":"idle","current_image_confirmed":true,"test_marker_present":false}\n');
      return;
    }
    if (request.method === "GET" && request.url === "/api/v1/ota") {
      if (!confirmed) {
        response.end('{"ok":true,"state":"pending_test","current_image_confirmed":false,"test_marker_present":true}\n');
      } else if (terminalState === "legacy") {
        response.end('{"ok":true,"state":"idle","current_image_confirmed":true}\n');
      } else {
        response.end(`{"ok":true,"state":"idle","current_image_confirmed":true,"test_marker_present":${markerPresent}}\n`);
      }
      return;
    }
    response.statusCode = 404;
    response.end('{"ok":false}\n');
  });

  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  assert.equal(typeof address, "object");
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "linkr-ota-hil-test-"));
  const image = path.join(tempDir, "fixture.bin");
  await writeFile(image, "fixture-image");

  try {
    try {
      const result = await execFileAsync("sh", [
        runner,
        "--flow", "api-manual-confirm",
        "--board-url", `http://127.0.0.1:${address.port}`,
        "--image", image,
        "--execute",
        "--allow-upload-test-reboot",
      ], { cwd: repoRoot });
      return { ...result, confirmRequests, exitCode: 0 };
    } catch (error) {
      return {
        stdout: error.stdout ?? "",
        stderr: error.stderr ?? "",
        confirmRequests,
        exitCode: error.code,
      };
    }
  } finally {
    await new Promise((resolve) => server.close(resolve));
    await rm(tempDir, { recursive: true, force: true });
  }
}

test("manual OTA flow accepts marker-qualified auto-confirm without observing pending_test", async () => {
  const result = await runManualConfirmFixture({ terminalState: "auto-confirmed" });
  assert.equal(result.confirmRequests, 0);
  assert.equal(result.exitCode, 0);
  assert.match(result.stdout, /firmware auto-confirmed the test image/);
});

test("manual OTA flow rejects marker-qualified rollback", async () => {
  const result = await runManualConfirmFixture({ terminalState: "rollback" });
  assert.equal(result.confirmRequests, 0);
  assert.equal(result.exitCode, 1);
  assert.match(result.stderr, /OTA test image rolled back/);
});

test("manual OTA flow rejects legacy idle-confirmed without marker evidence as inconclusive", async () => {
  const result = await runManualConfirmFixture({ terminalState: "legacy" });
  assert.equal(result.confirmRequests, 0);
  assert.equal(result.exitCode, 1);
  assert.match(result.stderr, /inconclusive OTA result/);
  assert.match(result.stderr, /does not expose test_marker_present/);
});

test("manual OTA flow still posts confirm when pending_test is observable", async () => {
  const result = await runManualConfirmFixture({});
  assert.equal(result.confirmRequests, 1);
  assert.equal(result.exitCode, 0);
  assert.match(result.stdout, /"state":"pending_test"/);
  assert.match(result.stdout, /"current_image_confirmed":true/);
});
