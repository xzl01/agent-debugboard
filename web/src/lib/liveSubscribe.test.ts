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

test("custom subscription clamps rate and batch size to the wire contract", () => {
  assert.deepEqual(liveSubscribeMessage(0, 0), {
    type: "subscribe",
    topic: "live",
    rate_hz: 1,
    batch_size: 1,
    id: "web",
  });
  assert.deepEqual(liveSubscribeMessage(5000, 500), {
    type: "subscribe",
    topic: "live",
    rate_hz: 1000,
    batch_size: TELEMETRY_STREAM_BATCH_SIZE,
    id: "web",
  });
});
