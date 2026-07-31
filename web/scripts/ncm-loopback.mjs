import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import readline from "node:readline";

const DEFAULT_NCM_HOST = "172.29.203.1";
const DEFAULT_RUBY = "/usr/bin/ruby";
const FORWARDER_SCRIPT = fileURLToPath(new URL("./ncm-forwarder.rb", import.meta.url));

export function shouldUseNcmLoopback(target, options = {}) {
  const url = new URL(target);
  const mode = options.mode ?? process.env.LINKR_NCM_FORWARDER ?? "auto";
  const platform = options.platform ?? process.platform;

  if (mode === "off") return false;
  if (options.force || mode === "force") return url.protocol === "http:";
  return platform === "darwin" &&
    url.protocol === "http:" &&
    url.hostname === DEFAULT_NCM_HOST;
}

function noopForwarder(target) {
  return {
    target,
    upstream: target,
    forwarded: false,
    exited: new Promise(() => {}),
    close: async () => {},
  };
}

function waitForReady(child, timeoutMs) {
  return new Promise((resolve, reject) => {
    const lines = readline.createInterface({ input: child.stdout });
    const timer = setTimeout(() => {
      cleanup();
      reject(new Error("USB-NCM loopback forwarder did not become ready"));
    }, timeoutMs);

    const onError = (error) => {
      cleanup();
      reject(error);
    };
    const onExit = (code, signal) => {
      cleanup();
      reject(new Error(
        `USB-NCM loopback forwarder exited before startup (${signal || code || "unknown"})`
      ));
    };
    const cleanup = () => {
      clearTimeout(timer);
      lines.removeAllListeners();
      lines.close();
      child.off("error", onError);
      child.off("exit", onExit);
    };

    child.once("error", onError);
    child.once("exit", onExit);
    lines.once("line", (line) => {
      try {
        const ready = JSON.parse(line);
        if (ready?.ready !== true || !Number.isInteger(ready.port)) {
          throw new Error(`invalid ready message: ${line}`);
        }
        cleanup();
        child.stdout.resume();
        resolve({ host: ready.host || "127.0.0.1", port: ready.port });
      } catch (error) {
        cleanup();
        reject(error);
      }
    });
  });
}

export async function startNcmLoopback(target, options = {}) {
  if (!shouldUseNcmLoopback(target, options)) return noopForwarder(target);

  const upstream = new URL(target);
  const targetPort = Number(upstream.port || 80);
  const rubyPath = options.rubyPath || DEFAULT_RUBY;
  const forwarderScript = options.forwarderScript || FORWARDER_SCRIPT;
  const spawnProcess = options.spawnProcess || spawn;
  const restartLimit = Math.max(0, Math.round(options.restartLimit ?? 3));
  const restartDelayMs = Math.max(0, Math.round(options.restartDelayMs ?? 100));
  const configuredPort = Number(options.listenPort ?? process.env.LINKR_NCM_FORWARDER_PORT ?? 0);
  let child = null;
  let closed = false;
  let restartCount = 0;
  let listenPort = Number.isInteger(configuredPort) && configuredPort >= 0 ? configuredPort : 0;
  let settleExit;
  let settled = false;
  const exited = new Promise((resolve) => {
    settleExit = (error) => {
      if (settled) return;
      settled = true;
      resolve(error);
    };
  });

  const startChild = async () => {
    const next = spawnProcess(
      rubyPath,
      [forwarderScript, upstream.hostname, String(targetPort)],
      {
        stdio: ["ignore", "pipe", "pipe"],
        env: {
          ...process.env,
          ...options.env,
          LINKR_NCM_FORWARDER_PORT: String(listenPort),
        },
      },
    );
    child = next;
    next.stderr.on("data", (chunk) => {
      process.stderr.write(`[ncm-forwarder] ${chunk}`);
    });
    try {
      const ready = await waitForReady(next, options.timeoutMs || 5_000);
      if (listenPort !== 0 && ready.port !== listenPort) {
        throw new Error(
          `USB-NCM loopback forwarder restarted on unexpected port ${ready.port}`,
        );
      }
      listenPort = ready.port;
      return { next, ready };
    } catch (error) {
      if (next.exitCode === null && next.signalCode === null) next.kill("SIGTERM");
      throw error;
    }
  };

  let first;
  try {
    first = await startChild();
  } catch (error) {
    throw error;
  }

  const restartAfterExit = async (reason) => {
    let lastError = reason;
    while (!closed && restartCount < restartLimit) {
      restartCount += 1;
      if (restartDelayMs > 0) {
        await new Promise((resolve) => setTimeout(resolve, restartDelayMs * restartCount));
      }
      if (closed) break;
      try {
        const restarted = await startChild();
        process.stderr.write(
          `[ncm-forwarder] restarted on ${restarted.ready.host}:${restarted.ready.port} ` +
          `(attempt ${restartCount}/${restartLimit})\n`,
        );
        options.onRestart?.(restartCount);
        monitor(restarted.next);
        return;
      } catch (error) {
        lastError = error instanceof Error ? error : new Error(String(error));
      }
    }
    if (closed) {
      settleExit(null);
      return;
    }
    settleExit(new Error(
      `USB-NCM loopback forwarder could not be recovered after ${restartCount} restart attempts: ${lastError.message}`,
    ));
  };

  function monitor(monitoredChild) {
    let handled = false;
    const handleFailure = (reason) => {
      if (handled) return;
      handled = true;
      monitoredChild.off("error", onError);
      monitoredChild.off("exit", onExit);
      if (closed) {
        settleExit(null);
        return;
      }
      void restartAfterExit(reason);
    };
    const onError = (error) => handleFailure(error);
    const onExit = (code, signal) => handleFailure(new Error(
      `USB-NCM loopback forwarder exited (${signal || code || "unknown"})`,
    ));
    monitoredChild.once("error", onError);
    monitoredChild.once("exit", onExit);
    if (monitoredChild.exitCode !== null || monitoredChild.signalCode !== null) {
      queueMicrotask(() => onExit(monitoredChild.exitCode, monitoredChild.signalCode));
    }
  }

  monitor(first.next);

  const loopback = new URL(upstream);
  loopback.hostname = first.ready.host;
  loopback.port = String(first.ready.port);

  return {
    target: loopback.toString().replace(/\/$/, ""),
    upstream: upstream.toString().replace(/\/$/, ""),
    forwarded: true,
    exited,
    close: async () => {
      if (closed) return;
      closed = true;
      const activeChild = child;
      if (!activeChild || activeChild.exitCode !== null || activeChild.signalCode !== null) {
        settleExit(null);
        return;
      }
      await new Promise((resolve) => {
        const timer = setTimeout(() => {
          activeChild.kill("SIGKILL");
          resolve();
        }, 1_000);
        activeChild.once("exit", () => {
          clearTimeout(timer);
          resolve();
        });
        activeChild.kill("SIGTERM");
      });
      settleExit(null);
    },
  };
}
