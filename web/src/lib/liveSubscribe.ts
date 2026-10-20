export const TELEMETRY_STREAM_BATCH_SIZE = 18;
export const MAX_ADC_TELEMETRY_RATE_HZ = 250;

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

export function validateAdcTelemetryRate(rateHz: number): void {
  if (!Number.isInteger(rateHz) || rateHz < 1 || rateHz > MAX_ADC_TELEMETRY_RATE_HZ) {
    throw new Error(`ADC telemetry rate must be a whole number from 1 to ${MAX_ADC_TELEMETRY_RATE_HZ} Hz`);
  }
}

export function liveSubscribeMessage(rateHz: number, batchSize: number): LiveSubscribeMessage {
  validateAdcTelemetryRate(rateHz);
  return {
    type: "subscribe",
    topic: "live",
    rate_hz: rateHz,
    batch_size: Math.max(1, Math.min(TELEMETRY_STREAM_BATCH_SIZE, Math.round(batchSize))),
    id: "web",
  };
}

export function defaultLiveSubscribeMessage(): LiveSubscribeMessage {
  return liveSubscribeMessage(LIVE_TELEMETRY_RATE_HZ, LIVE_TELEMETRY_BATCH_SIZE);
}
