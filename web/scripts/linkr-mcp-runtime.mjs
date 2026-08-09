import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

import { SerialBrokerClient } from "./serial-broker-client.mjs";

const DEFAULT_API_BASE = "http://127.0.0.1:8787/api/v1";
const DEFAULT_SERIAL_URL = "ws://127.0.0.1:8787/serial";
const DEFAULT_GATEWAY_TIMEOUT_MS = 8_000;
const GATEWAY_ENTRY = fileURLToPath(new URL("../serial-bridge.mjs", import.meta.url));
const GATEWAY_SCHEMA = "radxa-linkr-debugger-gateway.v1";

export class LinkrMcpRuntimeError extends Error {
  constructor(code, message, details = {}) {
    super(message);
    this.name = "LinkrMcpRuntimeError";
    this.code = code;
    this.details = details;
  }
}

function trimTrailingSlash(value) {
  return String(value).replace(/\/+$/, "");
}

function isLoopbackGateway(apiBase) {
  try {
    const url = new URL(apiBase);
    return (url.hostname === "127.0.0.1" || url.hostname === "localhost") && url.port === "8787";
  } catch {
    return false;
  }
}

function wait(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function gatewayHealthUrl(apiBase) {
  return new URL("/healthz", apiBase).toString();
}

export class LinkrMcpRuntime {
  constructor({
    apiBase = process.env.LINKR_MCP_API_BASE || DEFAULT_API_BASE,
    serialUrl = process.env.LINKR_MCP_SERIAL_URL || DEFAULT_SERIAL_URL,
    autostartGateway = process.env.LINKR_MCP_AUTOSTART_GATEWAY !== "0",
    gatewayTimeoutMs = DEFAULT_GATEWAY_TIMEOUT_MS,
    fetchImpl = fetch,
    spawnImpl = spawn,
    serialFactory,
    stderr = process.stderr,
  } = {}) {
    this.apiBase = trimTrailingSlash(apiBase);
    this.serialUrl = serialUrl;
    this.autostartGateway = Boolean(autostartGateway);
    this.gatewayTimeoutMs = gatewayTimeoutMs;
    this.fetchImpl = fetchImpl;
    this.spawnImpl = spawnImpl;
    this.serialFactory = serialFactory || (() => new SerialBrokerClient({ url: this.serialUrl }));
    this.stderr = stderr;
    this.gateway = null;
    this.gatewayReady = null;
    this.serialClient = null;
  }

  async probeGateway(timeoutMs = 800) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    timer.unref?.();
    try {
      const response = await this.fetchImpl(gatewayHealthUrl(this.apiBase), {
        signal: controller.signal,
        headers: { accept: "application/json" },
      });
      const data = await response.json().catch(() => null);
      if (response.ok) {
        return data?.schema === GATEWAY_SCHEMA && data?.ok === true;
      }
      // A gateway from before /healthz support identifies itself with this
      // loopback-only JSON response. Accept it without falling back to a board
      // /status request, which would consume the firmware's small HTTP pool.
      return isLoopbackGateway(this.apiBase) &&
        response.status === 404 &&
        data?.error?.message === "unknown gateway path";
    } catch {
      return false;
    } finally {
      clearTimeout(timer);
    }
  }

  startGateway() {
    if (!isLoopbackGateway(this.apiBase)) {
      throw new LinkrMcpRuntimeError(
        "gateway_autostart_unsupported",
        `automatic gateway startup is only supported for ${DEFAULT_API_BASE}; configure and start ${this.apiBase} manually`,
      );
    }
    const child = this.spawnImpl(process.execPath, [GATEWAY_ENTRY], {
      cwd: fileURLToPath(new URL("..", import.meta.url)),
      env: process.env,
      stdio: ["ignore", "pipe", "pipe"],
    });
    this.gateway = child;
    for (const stream of [child.stdout, child.stderr]) {
      stream?.on("data", (chunk) => this.stderr.write(`[linkr-gateway] ${chunk}`));
    }
    child.once("exit", (code, signal) => {
      if (this.gateway === child) this.gateway = null;
      if (code && code !== 0) {
        this.stderr.write(`[linkr-mcp] gateway exited with code ${code}${signal ? ` (${signal})` : ""}\n`);
      }
    });
  }

  async ensureGateway() {
    if (await this.probeGateway()) return;
    if (!this.autostartGateway) {
      throw new LinkrMcpRuntimeError(
        "gateway_unavailable",
        `Linkr device gateway is unavailable at ${this.apiBase}; run npm run device-bridge`,
      );
    }
    if (this.gatewayReady) return this.gatewayReady;
    this.gatewayReady = (async () => {
      if (!this.gateway) this.startGateway();
      const deadline = Date.now() + this.gatewayTimeoutMs;
      while (Date.now() < deadline) {
        if (await this.probeGateway(500)) return;
        if (!this.gateway) break;
        await wait(100);
      }
      throw new LinkrMcpRuntimeError(
        "gateway_start_failed",
        `Linkr device gateway did not become ready at ${this.apiBase}`,
      );
    })().finally(() => {
      this.gatewayReady = null;
    });
    return this.gatewayReady;
  }

  async boardRequest(path, { method = "GET", body } = {}) {
    await this.ensureGateway();
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 10_000);
    timer.unref?.();
    let response;
    try {
      response = await this.fetchImpl(`${this.apiBase}${path}`, {
        method,
        signal: controller.signal,
        headers: body === undefined ? undefined : { "content-type": "application/json" },
        body: body === undefined ? undefined : JSON.stringify(body),
      });
    } catch (error) {
      throw new LinkrMcpRuntimeError(
        error?.name === "AbortError" ? "board_timeout" : "board_unreachable",
        error instanceof Error ? error.message : String(error),
      );
    } finally {
      clearTimeout(timer);
    }

    const text = await response.text();
    let data;
    try {
      data = text ? JSON.parse(text) : {};
    } catch {
      throw new LinkrMcpRuntimeError(
        "invalid_board_response",
        `device returned HTTP ${response.status} with a non-JSON response`,
      );
    }
    if (!response.ok || data?.ok === false) {
      throw new LinkrMcpRuntimeError(
        data?.error?.code || `http_${response.status}`,
        data?.error?.message || `device returned HTTP ${response.status}`,
        { status: response.status, response: data },
      );
    }
    if (data?.schema !== "radxa-linkr-debugger.v1") {
      throw new LinkrMcpRuntimeError(
        "unsupported_board_schema",
        `expected radxa-linkr-debugger.v1 but received ${String(data?.schema || "no schema")}`,
      );
    }
    return data;
  }

  boardStatus() {
    return this.boardRequest("/status");
  }

  adcRead() {
    return this.boardRequest("/adc/read");
  }

  powerSet(name, state) {
    return this.boardRequest(`/power/${encodeURIComponent(name)}`, {
      method: "PUT",
      body: { state },
    });
  }

  switchRoute(name, route) {
    return this.boardRequest(`/switch/${encodeURIComponent(name)}`, {
      method: "PUT",
      body: { route },
    });
  }

  async serial() {
    await this.ensureGateway();
    if (!this.serialClient) this.serialClient = this.serialFactory();
    await this.serialClient.connect();
    return this.serialClient;
  }

  async serialDisconnect(channel) {
    if (!this.serialClient) return { channel, closed: true, already_disconnected: true };
    return this.serialClient.closeChannel(channel);
  }

  async close() {
    if (this.serialClient) {
      await this.serialClient.close();
      this.serialClient = null;
    }
    const child = this.gateway;
    this.gateway = null;
    if (child && child.exitCode == null && child.signalCode == null) child.kill("SIGTERM");
  }
}
