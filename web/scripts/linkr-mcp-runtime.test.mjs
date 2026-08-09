import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import test from "node:test";

import { LinkrMcpRuntime, LinkrMcpRuntimeError } from "./linkr-mcp-runtime.mjs";

class FakeChild extends EventEmitter {
  constructor() {
    super();
    this.stdout = new EventEmitter();
    this.stderr = new EventEmitter();
    this.exitCode = null;
    this.signalCode = null;
    this.killed = false;
  }
  kill(signal) {
    this.killed = true;
    this.signalCode = signal;
  }
}

test("runtime lazily starts the loopback gateway and preserves board envelopes", async () => {
  const child = new FakeChild();
  const requests = [];
  let call = 0;
  const runtime = new LinkrMcpRuntime({
    fetchImpl: async (url, init) => {
      call += 1;
      if (call === 1) throw new TypeError("connection refused");
      if (call === 2) return Response.json({
        schema: "radxa-linkr-debugger-gateway.v1",
        ok: true,
      });
      requests.push({ url, init });
      return Response.json({ schema: "radxa-linkr-debugger.v1", ok: true, command: "status" });
    },
    spawnImpl: () => child,
    stderr: { write() {} },
  });
  const status = await runtime.boardStatus();
  assert.equal(status.command, "status");
  assert.equal(call, 3);
  assert.equal(requests[0].url, "http://127.0.0.1:8787/api/v1/status");
  await runtime.close();
  assert.equal(child.killed, true);
});

test("runtime rejects unsupported firmware response schemas", async () => {
  const runtime = new LinkrMcpRuntime({
    autostartGateway: false,
    fetchImpl: async (url) => url.endsWith("/healthz")
      ? Response.json({ schema: "radxa-linkr-debugger-gateway.v1", ok: true })
      : Response.json({ ok: true, command: "status" }),
  });
  await assert.rejects(
    runtime.boardStatus(),
    (error) => error instanceof LinkrMcpRuntimeError && error.code === "unsupported_board_schema",
  );
});

test("runtime accepts the legacy loopback gateway signature without probing firmware", async () => {
  const requests = [];
  const runtime = new LinkrMcpRuntime({
    autostartGateway: false,
    fetchImpl: async (url) => {
      requests.push(url);
      if (url.endsWith("/healthz")) {
        return Response.json({ ok: false, error: { message: "unknown gateway path" } }, { status: 404 });
      }
      return Response.json({ schema: "radxa-linkr-debugger.v1", ok: true, command: "status" });
    },
  });
  await runtime.boardStatus();
  assert.deepEqual(requests, [
    "http://127.0.0.1:8787/healthz",
    "http://127.0.0.1:8787/api/v1/status",
  ]);
});

test("disconnect does not start a gateway when this MCP process has no serial session", async () => {
  const runtime = new LinkrMcpRuntime({
    fetchImpl: async () => { throw new Error("must not probe"); },
  });
  assert.deepEqual(await runtime.serialDisconnect("uart0"), {
    channel: "uart0",
    closed: true,
    already_disconnected: true,
  });
});

test("runtime does not autostart a custom remote gateway", async () => {
  const runtime = new LinkrMcpRuntime({
    apiBase: "http://example.test/api/v1",
    fetchImpl: async () => { throw new TypeError("offline"); },
  });
  await assert.rejects(
    runtime.boardStatus(),
    (error) => error instanceof LinkrMcpRuntimeError && error.code === "gateway_autostart_unsupported",
  );
});
