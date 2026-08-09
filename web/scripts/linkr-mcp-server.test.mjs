import assert from "node:assert/strict";
import test from "node:test";

import { Client, InMemoryTransport } from "@modelcontextprotocol/client";

import { createLinkrMcpServer, summarizeBoardStatus } from "./linkr-mcp-server.mjs";

class FakeSerial {
  constructor() {
    this.calls = [];
  }
  async open(channel, baud) {
    this.calls.push(["open", channel, baud]);
    return { channel, baud, cursor: 4 };
  }
  async status(channel) {
    this.calls.push(["status", channel]);
    return { channel, connected: true, baud: 115200, subscribers: 2, owner: null };
  }
  async read(channel, options) {
    this.calls.push(["read", channel, options]);
    return { channel, text: "ready", next_cursor: 9 };
  }
  async expect(channel, options) {
    this.calls.push(["expect", channel, options]);
    return { channel, matched: options.pattern, next_cursor: 10 };
  }
  async claim(channel, owner) {
    this.calls.push(["claim", channel, owner]);
    return { channel, owner };
  }
  async write(channel, text, options) {
    this.calls.push(["write", channel, text, options]);
    return { channel, bytes: text.length };
  }
  async release(channel) {
    this.calls.push(["release", channel]);
    return { channel };
  }
  async command(channel, options) {
    this.calls.push(["command", channel, options]);
    return { channel, output: { text: "Linux\n$ " } };
  }
  async shellCommand(channel, options) {
    this.calls.push(["shellCommand", channel, options]);
    return { channel, exit_code: 0, output: "Linux\n", next_cursor: 16 };
  }
  async login(channel, options) {
    this.calls.push(["login", channel, options]);
    return { channel, authenticated: true, password_required: true, next_cursor: 20 };
  }
  async closeChannel(channel) {
    this.calls.push(["close", channel]);
    return { channel, closed: true };
  }
}

class FakeRuntime {
  constructor() {
    this.calls = [];
    this.serialClient = new FakeSerial();
  }
  async boardStatus() {
    this.calls.push(["status"]);
    return {
      schema: "radxa-linkr-debugger.v1",
      ok: true,
      command: "status",
      project: "radxa-linkr-debugger",
      mcu: "rp2350",
      usb: "ncm-http",
      power_outputs: [{ name: "5v_out", state: "on", value: 1, signal: "GP05_5V_EN" }],
      switches: { vin: { route: "3.3v", routes: ["1.8v", "3.3v"] } },
      gpios: [{ name: "GP7", value: 0, direction: "input" }],
    };
  }
  async adcRead() {
    this.calls.push(["adc"]);
    return { schema: "radxa-linkr-debugger.v1", ok: true, readings: [] };
  }
  async powerSet(name, state) {
    this.calls.push(["power", name, state]);
    return { schema: "radxa-linkr-debugger.v1", ok: true, name, state };
  }
  async switchRoute(name, route) {
    this.calls.push(["switch", name, route]);
    return { schema: "radxa-linkr-debugger.v1", ok: true, name, route };
  }
  async serial() {
    return this.serialClient;
  }
  async serialDisconnect(channel) {
    return this.serialClient.closeChannel(channel);
  }
  async close() {
    this.calls.push(["close"]);
  }
}

async function createClient(runtime = new FakeRuntime()) {
  const server = createLinkrMcpServer(runtime);
  const client = new Client({ name: "linkr-mcp-test", version: "1.0.0" });
  const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
  await server.connect(serverTransport);
  await client.connect(clientTransport);
  return { client, runtime, server };
}

test("MCP lists bounded board and shared serial tools", async (t) => {
  const context = await createClient();
  t.after(async () => {
    await context.client.close();
    await context.server.close();
  });
  const { tools } = await context.client.listTools();
  assert.deepEqual(tools.map((tool) => tool.name).sort(), [
    "linkr_adc_read",
    "linkr_board_status",
    "linkr_power_set",
    "linkr_serial_command",
    "linkr_serial_connect",
    "linkr_serial_disconnect",
    "linkr_serial_expect",
    "linkr_serial_login",
    "linkr_serial_read",
    "linkr_serial_shell_command",
    "linkr_serial_status",
    "linkr_serial_write",
    "linkr_switch_route",
  ]);
});

test("MCP returns structured status and requires explicit hardware confirmation", async (t) => {
  const context = await createClient();
  t.after(async () => {
    await context.client.close();
    await context.server.close();
  });
  const status = await context.client.callTool({ name: "linkr_board_status", arguments: {} });
  assert.equal(status.isError, undefined);
  assert.equal(status.structuredContent.schema, "radxa-linkr-debugger.mcp.v1");
  assert.equal(status.structuredContent.result.command, "status");
  assert.equal(status.structuredContent.result.gpios, undefined);
  assert.deepEqual(status.structuredContent.result.power_outputs, [{ name: "5v_out", state: "on", value: 1 }]);

  const full = await context.client.callTool({
    name: "linkr_board_status",
    arguments: { detail: "full" },
  });
  assert.equal(full.structuredContent.result.gpios[0].name, "GP7");

  const unconfirmed = await context.client.callTool({
    name: "linkr_power_set",
    arguments: { name: "5v_out", state: "off", confirm: false },
  });
  assert.equal(unconfirmed.isError, true);
  assert.deepEqual(context.runtime.calls, [["status"], ["status"]]);
  const power = await context.client.callTool({
    name: "linkr_power_set",
    arguments: { name: "5v_out", state: "off", confirm: true },
  });
  assert.equal(power.structuredContent.result.state, "off");
  assert.deepEqual(context.runtime.calls, [["status"], ["status"], ["power", "5v_out", "off"]]);
});

test("MCP serial tools preserve incremental and short-claim semantics", async (t) => {
  const context = await createClient();
  t.after(async () => {
    await context.client.close();
    await context.server.close();
  });
  const connected = await context.client.callTool({
    name: "linkr_serial_connect",
    arguments: { channel: "uart1", baud: 1_500_000 },
  });
  assert.equal(connected.structuredContent.result.cursor, 4);

  const status = await context.client.callTool({
    name: "linkr_serial_status",
    arguments: { channel: "uart1" },
  });
  assert.equal(status.structuredContent.result.subscribers, 2);

  const read = await context.client.callTool({
    name: "linkr_serial_read",
    arguments: { channel: "uart1", cursor: 4, max_chars: 100, wait_ms: 0 },
  });
  assert.equal(read.structuredContent.result.next_cursor, 9);

  await context.client.callTool({
    name: "linkr_serial_write",
    arguments: { channel: "uart1", text: "help", line_ending: "cr", exclusive: true },
  });
  assert.deepEqual(context.runtime.serialClient.calls.slice(-3).map((call) => call[0]), ["claim", "write", "release"]);

  const login = await context.client.callTool({
    name: "linkr_serial_login",
    arguments: { channel: "uart1", username: "root", password: "secret" },
  });
  assert.equal(login.structuredContent.result.authenticated, true);
  assert.equal(JSON.stringify(login.structuredContent).includes("secret"), false);

  const shell = await context.client.callTool({
    name: "linkr_serial_shell_command",
    arguments: { channel: "uart1", command: "uname -a" },
  });
  assert.equal(shell.structuredContent.result.exit_code, 0);
});

test("status summary omits bulky GPIO and route metadata", () => {
  const full = {
    schema: "radxa-linkr-debugger.v1",
    ok: true,
    command: "status",
    power_outputs: [{ name: "5v_out", state: "off", value: 0, signal: "GP05_5V_EN" }],
    switches: { sd: { route: "target", routes: ["target", "usb-reader"], requires_confirm: false } },
    gpios: Array.from({ length: 20 }, (_, pin) => ({ name: `GP${pin}`, pin, direction: "input", value: 0 })),
    board_monitoring: {
      memory: {
        available: true,
        current_pressure: { pressure_pct_x100: 100 },
        peak_pressure: { pressure_pct_x100: 200 },
        stacks: { thread_count: 99 },
      },
    },
  };
  const summary = summarizeBoardStatus(full);
  assert.equal("gpios" in summary, false);
  assert.deepEqual(summary.switches, { sd: { route: "target" } });
  assert.deepEqual(summary.board_monitoring.memory, {
    available: true,
    current_pressure_pct_x100: 100,
    peak_pressure_pct_x100: 200,
  });
});
