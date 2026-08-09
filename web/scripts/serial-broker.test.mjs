import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import test from "node:test";

import { SerialBroker, selectSerialBrokerPort } from "./serial-broker.mjs";

class FakeSerialPort extends EventEmitter {
  constructor(options, writes) {
    super();
    this.path = options.path;
    this.baudRate = options.baudRate;
    this.isOpen = false;
    this.writes = writes;
  }

  open(callback) {
    this.isOpen = true;
    queueMicrotask(() => callback());
  }

  write(data, callback) {
    this.writes.push({ path: this.path, text: data.toString("utf8") });
    queueMicrotask(() => callback());
  }

  drain(callback) {
    queueMicrotask(() => callback());
  }

  close(callback) {
    if (!this.isOpen) {
      queueMicrotask(() => callback());
      return;
    }
    this.isOpen = false;
    queueMicrotask(() => {
      this.emit("close");
      callback();
    });
  }
}

function createHarness(options = {}) {
  const ports = [
    { path: "/dev/cu.wchusbserial-D1", vendorId: "1A86" },
    { path: "/dev/cu.wchusbserial-D3", vendorId: "1a86" },
  ];
  const created = [];
  const writes = [];
  const frames = new Map();
  const send = (peer, frame) => {
    const peerFrames = frames.get(peer) || [];
    peerFrames.push(frame);
    frames.set(peer, peerFrames);
  };
  const broker = new SerialBroker({
    listPorts: async () => ports,
    createPort: (serialOptions) => {
      const serial = new FakeSerialPort(serialOptions, writes);
      created.push(serial);
      return serial;
    },
    send,
    idleTimeoutMs: options.idleTimeoutMs ?? 10,
    nowMonoUs: () => 123_456,
  });
  const peer = (id) => ({ id, __brokerClientId: id });
  return { broker, created, frames, peer, ports, writes };
}

function frameOf(frames, peer, type) {
  return (frames.get(peer) || []).findLast((frame) => frame.type === type);
}

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

test("maps CH347F D1 and D3 ports to independent broker channels", () => {
  const ports = [
    { path: "/dev/cu.wchusbserial-D3", vendorId: "1a86" },
    { path: "/dev/cu.wchusbserial-D1", vendorId: "1a86" },
    { path: "/dev/cu.other", vendorId: "ffff" },
  ];
  assert.equal(selectSerialBrokerPort(ports, "uart0")?.path, "/dev/cu.wchusbserial-D1");
  assert.equal(selectSerialBrokerPort(ports, "uart1")?.path, "/dev/cu.wchusbserial-D3");
});

test("shares one UART connection and broadcasts RX to every subscriber", async () => {
  const { broker, created, frames, peer } = createHarness();
  const web = peer("web");
  const mcp = peer("mcp");

  assert.equal(await broker.subscribe(web, { channel: "uart0", baud: 115200, request_id: "open-web" }), true);
  assert.equal(await broker.subscribe(mcp, { channel: "uart0", baud: 115200, request_id: "open-mcp" }), true);
  assert.equal(created.length, 1);
  assert.equal(frameOf(frames, web, "opened")?.shared, true);
  assert.equal(frameOf(frames, mcp, "status")?.subscribers, 2);

  created[0].emit("data", Buffer.from("ready，完成。", "utf8"));
  assert.equal(frameOf(frames, web, "data")?.text, "ready，完成。");
  assert.equal(frameOf(frames, mcp, "data")?.host_t_mono_us, 123_456);
  assert.equal(frameOf(frames, web, "data")?.byte_count, Buffer.byteLength("ready，完成。"));

  await broker.shutdown();
});

test("keeps MCP shell bookkeeping private while observers receive command output", async () => {
  const { broker, created, frames, peer } = createHarness();
  const web = peer("web");
  const mcp = peer("mcp");
  const token = "eca90be0b06e496c9a410d875af47976";
  await broker.subscribe(web, { channel: "uart0", baud: 115200 });
  await broker.subscribe(mcp, { channel: "uart0", baud: 115200 });
  assert.equal(broker.claim(mcp, { channel: "uart0", owner: "mcp-shell-command" }), true);
  assert.equal(await broker.write(mcp, {
    channel: "uart0",
    data: { encoding: "utf8", value: "uname -a\r" },
    observer_redaction: { line_token: token },
  }), true);

  created[0].emit("data", Buffer.from(
    `root@target:~$ uname -a\r\nroot@target:~$ __linkr_rc_${token.slice(0, 8)}`,
  ));
  created[0].emit("data", Buffer.from(
    `${token.slice(8)}=$?\r\nroot@target:~$ printf '__LINKR_RC_${token}__%d\\n' "$__linkr_rc_${token}"\r\nLinux target\r\n__LINKR_RC_${token}__0\r\nroot@target:~$ `,
  ));
  broker.release(mcp, { channel: "uart0" });

  const webText = (frames.get(web) || [])
    .filter((frame) => frame.type === "data")
    .map((frame) => frame.text)
    .join("");
  const mcpText = (frames.get(mcp) || [])
    .filter((frame) => frame.type === "data")
    .map((frame) => frame.text)
    .join("");
  assert.equal(webText, "root@target:~$ uname -a\r\nLinux target\r\nroot@target:~$ ");
  assert.equal(webText.includes("LINKR_RC"), false);
  assert.equal(webText.includes("linkr_rc"), false);
  assert.equal(mcpText.includes(`__LINKR_RC_${token}__0`), true);
  assert.equal(mcpText.includes(`__linkr_rc_${token}=$?`), true);
  await broker.shutdown();
});

test("requires an exclusive claim before observer redaction", async () => {
  const { broker, frames, peer } = createHarness();
  const mcp = peer("mcp");
  await broker.subscribe(mcp, { channel: "uart0", baud: 115200 });
  assert.equal(await broker.write(mcp, {
    channel: "uart0",
    request_id: "private-write",
    data: { encoding: "utf8", value: "x" },
    observer_redaction: { line_token: "eca90be0b06e496c9a410d875af47976" },
  }), false);
  assert.equal(frameOf(frames, mcp, "error")?.code, "serial_claim_required");
  await broker.shutdown();
});

test("attributes split UTF-8 bytes to the data frame that emits the character", async () => {
  const { broker, created, frames, peer } = createHarness();
  const web = peer("web");
  await broker.subscribe(web, { channel: "uart0", baud: 115200 });
  const bytes = Buffer.from("A，B", "utf8");
  created[0].emit("data", bytes.subarray(0, 3));
  created[0].emit("data", bytes.subarray(3));
  const dataFrames = (frames.get(web) || []).filter((frame) => frame.type === "data");
  assert.deepEqual(dataFrames.map((frame) => [frame.text, frame.byte_count]), [
    ["A", 1],
    ["，B", 4],
  ]);
  await broker.shutdown();
});

test("opens UART0 and UART1 independently", async () => {
  const { broker, created, peer } = createHarness();
  await broker.subscribe(peer("zero"), { channel: "uart0", baud: 115200 });
  await broker.subscribe(peer("one"), { channel: "uart1", baud: 1500000 });
  assert.deepEqual(created.map((port) => [port.path, port.baudRate]), [
    ["/dev/cu.wchusbserial-D1", 115200],
    ["/dev/cu.wchusbserial-D3", 1500000],
  ]);
  await broker.shutdown();
});

test("serializes writes and acknowledges each completed write", async () => {
  const { broker, frames, peer, writes } = createHarness();
  const web = peer("web");
  await broker.subscribe(web, { channel: "uart0", baud: 115200 });

  await Promise.all([
    broker.write(web, { channel: "uart0", request_id: "w1", data: { encoding: "utf8", value: "first" } }),
    broker.write(web, { channel: "uart0", request_id: "w2", data: { encoding: "utf8", value: "second" } }),
  ]);
  assert.deepEqual(writes.map((entry) => entry.text), ["first", "second"]);
  assert.deepEqual(
    (frames.get(web) || []).filter((frame) => frame.type === "write_ack").map((frame) => frame.request_id),
    ["w1", "w2"],
  );
  await broker.shutdown();
});

test("exclusive claim blocks other writers until release", async () => {
  const { broker, frames, peer, writes } = createHarness();
  const automation = peer("automation");
  const web = peer("web");
  await broker.subscribe(automation, { channel: "uart0", baud: 115200 });
  await broker.subscribe(web, { channel: "uart0", baud: 115200 });

  assert.equal(broker.claim(automation, { channel: "uart0", owner: "test-run", request_id: "claim" }), true);
  assert.equal(await broker.write(web, {
    channel: "uart0",
    request_id: "blocked",
    data: { encoding: "utf8", value: "no" },
  }), false);
  assert.equal(frameOf(frames, web, "error")?.code, "serial_busy");
  assert.equal(writes.length, 0);

  broker.release(automation, { channel: "uart0", request_id: "release" });
  assert.equal(await broker.write(web, {
    channel: "uart0",
    request_id: "allowed",
    data: { encoding: "utf8", value: "yes" },
  }), true);
  assert.equal(writes[0].text, "yes");
  await broker.shutdown();
});

test("rejects a conflicting baud rate without disturbing active subscribers", async () => {
  const { broker, created, frames, peer } = createHarness();
  const first = peer("first");
  const second = peer("second");
  await broker.subscribe(first, { channel: "uart0", baud: 115200 });
  assert.equal(await broker.subscribe(second, {
    channel: "uart0",
    baud: 921600,
    request_id: "conflict",
  }), false);
  assert.equal(frameOf(frames, second, "error")?.code, "baud_conflict");
  assert.equal(created.length, 1);
  assert.equal(created[0].isOpen, true);
  await broker.shutdown();
});

test("keeps a dropped client's port briefly, then closes it when idle", async () => {
  const { broker, created, peer } = createHarness({ idleTimeoutMs: 5 });
  const web = peer("web");
  await broker.subscribe(web, { channel: "uart0", baud: 115200 });
  await broker.disconnectPeer(web);
  assert.equal(created[0].isOpen, true);
  await delay(15);
  assert.equal(created[0].isOpen, false);
  await broker.shutdown();
});
