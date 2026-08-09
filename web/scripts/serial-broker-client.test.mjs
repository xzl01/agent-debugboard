import assert from "node:assert/strict";
import test from "node:test";

import { WebSocketServer } from "ws";

import { SerialBrokerClient, SerialBrokerClientError } from "./serial-broker-client.mjs";
import { parseSerialBrokerClientFrame, serialBrokerFrame } from "./serial-broker-protocol.mjs";

async function createFakeBroker({ replyForWrite } = {}) {
  const wss = new WebSocketServer({ port: 0 });
  await new Promise((resolve) => wss.once("listening", resolve));
  const address = wss.address();
  const requests = [];
  wss.on("connection", (ws) => {
    ws.send(JSON.stringify(serialBrokerFrame("hello", {
      server: "test-broker",
      client_id: "test-client",
      capabilities: {},
    })));
    ws.on("message", (raw) => {
      const parsed = parseSerialBrokerClientFrame(raw.toString());
      assert.equal(parsed.ok, true);
      const request = parsed.message;
      requests.push(request);
      const common = { request_id: request.request_id, channel: request.channel };
      switch (request.type) {
        case "open":
          ws.send(JSON.stringify(serialBrokerFrame("opened", { ...common, path: "/dev/test", baud: request.baud })));
          break;
        case "status":
          ws.send(JSON.stringify(serialBrokerFrame("status", { ...common, connected: true, baud: 115200 })));
          break;
        case "claim":
          ws.send(JSON.stringify(serialBrokerFrame("claimed", { ...common, owner: request.owner })));
          break;
        case "release":
          ws.send(JSON.stringify(serialBrokerFrame("released", common)));
          break;
        case "write":
          ws.send(JSON.stringify(serialBrokerFrame("write_ack", { ...common, bytes: request.data.value.length })));
          const reply = replyForWrite
            ? replyForWrite(request.data.value, requests)
            : `${request.data.value}done$ `;
          ws.send(JSON.stringify(serialBrokerFrame("data", {
            channel: request.channel,
            sequence: requests.length,
            host_t_mono_us: 1234,
            text: reply,
            byte_count: Buffer.byteLength(reply),
          })));
          break;
        case "close":
          ws.send(JSON.stringify(serialBrokerFrame("closed", { ...common, reason: "client unsubscribed" })));
          break;
      }
    });
  });
  return {
    url: `ws://127.0.0.1:${address.port}`,
    requests,
    close: () => new Promise((resolve) => wss.close(resolve)),
  };
}

test("broker client keeps cursor-based history and supports command claims", async (t) => {
  const broker = await createFakeBroker();
  const client = new SerialBrokerClient({ url: broker.url, requestTimeoutMs: 1_000 });
  t.after(async () => {
    await client.close();
    await broker.close();
  });

  assert.equal(await client.connect(), "test-client");
  const opened = await client.open("uart0", 115200);
  assert.equal(opened.path, "/dev/test");
  const cursor = client.currentCursor("uart0");
  const command = await client.command("uart0", {
    command: "uname -a",
    prompt: "$ ",
    timeoutMs: 1_000,
  });
  assert.match(command.output.text, /uname -a\rdone\$ /);
  assert.equal(command.output.match_cursor >= cursor, true);

  const remainder = await client.read("uart0", { cursor, maxChars: 100 });
  assert.match(remainder.text, /done\$ /);
  assert.equal(remainder.next_cursor, remainder.latest_cursor);
  assert.deepEqual(
    broker.requests.filter((request) => ["claim", "write", "release"].includes(request.type)).map((request) => request.type),
    ["claim", "write", "release"],
  );
});

test("broker client rejects expired cursors and invalid regular expressions", async (t) => {
  const broker = await createFakeBroker();
  const client = new SerialBrokerClient({
    url: broker.url,
    requestTimeoutMs: 1_000,
    maxHistoryChars: 8,
  });
  t.after(async () => {
    await client.close();
    await broker.close();
  });

  await client.open("uart1", 115200);
  await client.write("uart1", "123456789", { lineEnding: "none" });
  assert.throws(
    () => client.collect("uart1", 0, 100),
    (error) => error instanceof SerialBrokerClientError && error.code === "serial_cursor_expired",
  );
  assert.throws(
    () => client.collect("uart1", 5_000, 100),
    (error) => error instanceof SerialBrokerClientError &&
      error.code === "serial_cursor_ahead" &&
      error.details.latest_cursor === client.currentCursor("uart1"),
  );
  await assert.rejects(
    client.expect("uart1", { pattern: "[", regex: true, timeoutMs: 10 }),
    (error) => error instanceof SerialBrokerClientError && error.code === "invalid_regex",
  );
});

test("broker client performs a claimed login flow without returning the password", async (t) => {
  const broker = await createFakeBroker({
    replyForWrite(value) {
      if (value === "\r") return "target login: ";
      if (value === "root\r") return "Password: ";
      if (value === "secret\r") return "\r\nroot@target:~# ";
      return "";
    },
  });
  const client = new SerialBrokerClient({ url: broker.url, requestTimeoutMs: 1_000 });
  t.after(async () => {
    await client.close();
    await broker.close();
  });

  await client.open("uart0", 115200);
  const result = await client.login("uart0", {
    username: "root",
    password: "secret",
    timeoutMs: 1_000,
  });
  assert.deepEqual(result, {
    channel: "uart0",
    authenticated: true,
    password_required: true,
    next_cursor: result.next_cursor,
  });
  assert.equal(JSON.stringify(result).includes("secret"), false);
  assert.deepEqual(
    broker.requests.filter((request) => ["claim", "write", "release"].includes(request.type)).map((request) => request.type),
    ["claim", "write", "write", "write", "release"],
  );
});

test("broker client reports POSIX shell exit status and releases its claim", async (t) => {
  const broker = await createFakeBroker({
    replyForWrite(value) {
      const marker = /(__LINKR_RC_[A-Za-z0-9_]+__)/.exec(value)?.[1];
      const token = marker?.slice("__LINKR_RC_".length, -2);
      return marker
        ? `root@target:~# __linkr_rc_${token}=$?\nroot@target:~# printf '${marker}%d\\n' "$__linkr_rc_${token}"\ncommand output\n${marker}0\nroot@target:~# `
        : "";
    },
  });
  const client = new SerialBrokerClient({ url: broker.url, requestTimeoutMs: 1_000 });
  t.after(async () => {
    await client.close();
    await broker.close();
  });

  await client.open("uart0", 115200);
  const result = await client.shellCommand("uart0", {
    command: "uname -a",
    prompt: "[#$>]\\s*$",
    timeoutMs: 1_000,
  });
  assert.equal(result.exit_code, 0);
  assert.match(result.output, /command output/);
  assert.equal(result.output.includes("LINKR_RC"), false);
  assert.equal(result.output.includes("linkr_rc"), false);
  const write = broker.requests.find((request) => request.type === "write");
  assert.match(write.observer_redaction.line_token, /^[A-Za-z0-9]{8,64}$/);
  assert.deepEqual(
    broker.requests.filter((request) => ["claim", "write", "release"].includes(request.type)).map((request) => request.type),
    ["claim", "write", "release"],
  );
});
