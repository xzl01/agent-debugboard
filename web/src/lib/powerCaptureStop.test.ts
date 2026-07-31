import assert from "node:assert/strict";
import test from "node:test";

import { createPowerCaptureStopHandshake } from "./powerCaptureStop.ts";

test("accepts tail telemetry after stop is requested until the matching ACK", async () => {
  const stop = createPowerCaptureStopHandshake("stop-1", 1_000);
  const accepted: number[] = [];
  const receive = (sequence: number) => {
    if (stop.acceptsTelemetry) accepted.push(sequence);
  };

  receive(100);
  assert.equal(stop.acknowledge("another-request"), false);
  receive(101);
  assert.equal(stop.acknowledge("stop-1"), true);
  await stop.promise;
  receive(102);

  assert.deepEqual(accepted, [100, 101]);
  assert.equal(stop.acceptsTelemetry, false);
});

test("keeps stop request IDs within the firmware 32-byte command buffer", async () => {
  const stop = createPowerCaptureStopHandshake("web-stop-4294967295", 1_000);
  assert.ok(new TextEncoder().encode(stop.requestId).byteLength < 32);
  stop.acknowledge(stop.requestId);
  await stop.promise;
});

test("rejects the stop handshake on timeout and closes the telemetry gate", async () => {
  const stop = createPowerCaptureStopHandshake("stop-timeout", 5);
  await assert.rejects(stop.promise, /Timed out waiting.*capture_stop/);
  assert.equal(stop.acceptsTelemetry, false);
});
