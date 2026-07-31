import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { startNcmLoopback } from "./ncm-loopback.mjs";

const webRoot = fileURLToPath(new URL("../", import.meta.url));
const viteBin = fileURLToPath(new URL("../node_modules/vite/bin/vite.js", import.meta.url));
// Use the firmware's native control service. Port 80 is a compatibility relay
// for the board-hosted page and only supports a small number of simultaneous
// connections, which is insufficient for multiple live WebSocket streams.
const configuredTarget = process.env.VITE_DEBUGGER_TARGET || "http://172.29.203.1:8080";

let forwarder;
try {
  forwarder = await startNcmLoopback(configuredTarget);
} catch (error) {
  console.error(`Cannot start USB-NCM loopback forwarder: ${error.message}`);
  process.exit(1);
}

if (forwarder.forwarded) {
  console.log(`USB-NCM loopback: ${forwarder.target} -> ${forwarder.upstream}`);
}

const vite = spawn(
  process.execPath,
  [viteBin, "--config", "vite.config.ts", ...process.argv.slice(2)],
  {
    cwd: webRoot,
    stdio: "inherit",
    env: {
      ...process.env,
      VITE_DEBUGGER_TARGET: forwarder.target,
    },
  }
);

let stopping = false;
let forwarderFailure = null;
const stop = (signal) => {
  if (stopping) return;
  stopping = true;
  if (vite.exitCode === null && vite.signalCode === null) vite.kill(signal);
  void forwarder.close();
};

process.once("SIGINT", () => stop("SIGINT"));
process.once("SIGTERM", () => stop("SIGTERM"));

void forwarder.exited.then((error) => {
  if (!error || stopping) return;
  forwarderFailure = error;
  console.error(error.message);
  stop("SIGTERM");
});

vite.once("error", async (error) => {
  console.error(`Cannot start Vite: ${error.message}`);
  await forwarder.close();
  process.exitCode = 1;
});

vite.once("exit", async (code, signal) => {
  await forwarder.close();
  if (forwarderFailure) {
    process.exitCode = 1;
    return;
  }
  if (signal) {
    process.kill(process.pid, signal);
    return;
  }
  process.exitCode = code ?? 1;
});
