import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const [shellInstaller, powershellInstaller] = await Promise.all([
  readFile(new URL("./install-host.sh", import.meta.url), "utf8"),
  readFile(new URL("./install-host.ps1", import.meta.url), "utf8"),
]);

test("desktop installers use the daemon data slug and graceful shutdown request", () => {
  assert.match(shellInstaller, /tray_data_dir=.*radxa-linkr-debugger/);
  assert.match(shellInstaller, /tray_shutdown_request=.*shutdown\.request/);
  assert.match(shellInstaller, />"\$tray_shutdown_request"/);
  assert.match(powershellInstaller, /Join-Path \$env:LOCALAPPDATA "radxa-linkr-debugger"/);
  assert.match(powershellInstaller, /Set-Content -LiteralPath \$trayShutdownRequest/);
});
