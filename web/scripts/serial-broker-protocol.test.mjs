import assert from "node:assert/strict";
import test from "node:test";
import {
  SERIAL_BROKER_PROTOCOL,
  parseSerialBrokerClientFrame,
  parseSerialBrokerServerFrame,
  serialBrokerClaimRequest,
  serialBrokerOpenRequest,
  serialBrokerReleaseRequest,
  serialBrokerWriteRequest,
} from "./serial-broker-protocol.mjs";

test("creates versioned open and write requests", () => {
  assert.deepEqual(serialBrokerOpenRequest("uart1", 1_500_000, "open-1"), {
    protocol: SERIAL_BROKER_PROTOCOL,
    type: "open",
    request_id: "open-1",
    channel: "uart1",
    baud: 1_500_000,
  });
  assert.deepEqual(serialBrokerWriteRequest("uart0", "uname -a\r", "write-1"), {
    protocol: SERIAL_BROKER_PROTOCOL,
    type: "write",
    request_id: "write-1",
    channel: "uart0",
    data: { encoding: "utf8", value: "uname -a\r" },
  });
  assert.deepEqual(
    serialBrokerWriteRequest("uart0", "uname -a\r", "write-private", {
      observerRedactionToken: "eca90be0b06e496c9a410d875af47976",
    }),
    {
      protocol: SERIAL_BROKER_PROTOCOL,
      type: "write",
      request_id: "write-private",
      channel: "uart0",
      data: { encoding: "utf8", value: "uname -a\r" },
      observer_redaction: { line_token: "eca90be0b06e496c9a410d875af47976" },
    },
  );
  assert.deepEqual(serialBrokerClaimRequest("uart0", "web automation", "claim-1"), {
    protocol: SERIAL_BROKER_PROTOCOL,
    type: "claim",
    request_id: "claim-1",
    channel: "uart0",
    owner: "web automation",
  });
  assert.deepEqual(serialBrokerReleaseRequest("uart0", "release-1"), {
    protocol: SERIAL_BROKER_PROTOCOL,
    type: "release",
    request_id: "release-1",
    channel: "uart0",
  });
});

test("rejects unversioned and raw legacy frames", () => {
  const open = parseSerialBrokerClientFrame(JSON.stringify({
    type: "open",
    channel: "uart1",
    baud: 115200,
  }));
  assert.equal(open.ok, false);
  assert.equal(open.error.code, "unsupported_protocol");

  const write = parseSerialBrokerClientFrame("legacy command\r");
  assert.equal(write.ok, false);
  assert.equal(write.error.code, "invalid_json");
});

test("rejects unsupported versions, invalid baud rates, and malformed writes", () => {
  const version = parseSerialBrokerClientFrame(JSON.stringify({
    protocol: "linkr-serial-broker.v99",
    type: "status",
    request_id: "status-1",
  }));
  assert.equal(version.ok, false);
  assert.equal(version.error.code, "unsupported_protocol");
  assert.equal(version.error.request_id, "status-1");

  const channel = parseSerialBrokerClientFrame(JSON.stringify({
    protocol: SERIAL_BROKER_PROTOCOL,
    type: "status",
    channel: "uart2",
  }));
  assert.equal(channel.ok, false);
  assert.equal(channel.error.code, "invalid_channel");

  const baud = parseSerialBrokerClientFrame(JSON.stringify({
    protocol: SERIAL_BROKER_PROTOCOL,
    type: "open",
    channel: "uart0",
    baud: 0,
  }));
  assert.equal(baud.ok, false);
  assert.match(baud.error.message, /baud/i);

  const write = parseSerialBrokerClientFrame(JSON.stringify({
    protocol: SERIAL_BROKER_PROTOCOL,
    type: "write",
    channel: "uart0",
    data: { encoding: "binary", value: "00" },
  }));
  assert.equal(write.ok, false);
  assert.equal(write.error.code, "invalid_write");

  const redaction = parseSerialBrokerClientFrame(JSON.stringify({
    protocol: SERIAL_BROKER_PROTOCOL,
    type: "write",
    channel: "uart0",
    data: { encoding: "utf8", value: "x" },
    observer_redaction: { line_token: "bad token" },
  }));
  assert.equal(redaction.ok, false);
  assert.equal(redaction.error.code, "invalid_write");
});

test("parses compatible server frames and ignores foreign protocol frames", () => {
  assert.equal(parseSerialBrokerServerFrame(JSON.stringify({
    protocol: SERIAL_BROKER_PROTOCOL,
    type: "hello",
    client_id: "client-1",
  }))?.client_id, "client-1");
  assert.equal(parseSerialBrokerServerFrame(JSON.stringify({
    protocol: "other.v1",
    type: "hello",
  })), null);
  assert.equal(parseSerialBrokerServerFrame(JSON.stringify({
    type: "opened",
    channel: "uart0",
  })), null);
  assert.equal(parseSerialBrokerServerFrame("not-json"), null);
});
