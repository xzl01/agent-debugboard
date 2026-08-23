export const TELEMETRY_STREAM_BATCH_SIZE = 20;

// The default live subscription requests a 60 Hz wire stream with one sample
// per WebSocket frame. The UI republishes the newest readings once per
// animation frame, so wire cadence and UI cadence are decoupled.
const LIVE_TELEMETRY_RATE_HZ = 60;
const LIVE_TELEMETRY_BATCH_SIZE = 1;

export interface LiveSubscribeMessage {
  type: "subscribe";
  topic: "live";
  rate_hz: number;
  batch_size: number;
  id: "web";
}

export function liveSubscribeMessage(rateHz: number, batchSize: number): LiveSubscribeMessage {
  return {
    type: "subscribe",
    topic: "live",
    rate_hz: Math.max(1, Math.min(1000, Math.round(rateHz))),
    batch_size: Math.max(1, Math.min(TELEMETRY_STREAM_BATCH_SIZE, Math.round(batchSize))),
    id: "web",
  };
}

export function defaultLiveSubscribeMessage(): LiveSubscribeMessage {
  return liveSubscribeMessage(LIVE_TELEMETRY_RATE_HZ, LIVE_TELEMETRY_BATCH_SIZE);
}
