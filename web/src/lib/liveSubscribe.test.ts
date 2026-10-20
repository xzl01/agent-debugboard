import assert from "node:assert/strict";
import test from "node:test";
import {
  defaultLiveSubscribeMessage,
  liveSubscribeMessage,
  TELEMETRY_STREAM_BATCH_SIZE,
} from "./liveSubscribe.ts";

test("default live telemetry subscription requests a 60 Hz wire stream with one sample per frame", () => {
  // batch_size 1 keeps each 60 Hz wire sample in its own WebSocket frame.
  // The UI republishes previews at animation-frame cadence; it no longer
  // consumes one UI update per wire frame.
  assert.deepEqual(defaultLiveSubscribeMessage(), {
    type: "subscribe",
    topic: "live",
    rate_hz: 60,
    batch_size: 1,
    id: "web",
  });
});

test("custom subscription accepts the 250 Hz wire maximum and clamps batch size", () => {
  assert.deepEqual(liveSubscribeMessage(1, 0), {
    type: "subscribe",
    topic: "live",
    rate_hz: 1,
    batch_size: 1,
    id: "web",
  });
  assert.deepEqual(liveSubscribeMessage(250, 500), {
    type: "subscribe",
    topic: "live",
    rate_hz: 250,
    batch_size: TELEMETRY_STREAM_BATCH_SIZE,
    id: "web",
  });
});

test("invalid rates cannot produce subscription requests, including the obsolete 500 Hz rate", () => {
  for (const rate of [0, -1, 1.5, 251, 500, 1000, NaN, Infinity]) {
    assert.throws(() => liveSubscribeMessage(rate, 20), /whole number from 1 to 250 Hz/);
  }
});
