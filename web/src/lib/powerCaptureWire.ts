import { parseCaptureCurrentReadings } from "./adc.ts";
import { POWER_CAPTURE_PROTOCOL } from "./powerCapture.ts";
import type {
  CaptureConfig,
  CaptureSample,
  PowerCapture,
} from "./types.ts";

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

export type LegacyCaptureBuilder = Omit<PowerCapture, "samples" | "capturedAt"> & {
  samples: CaptureSample[];
  expected: number;
};

export function createLegacyCaptureBuilder(
  message: Record<string, unknown>,
): LegacyCaptureBuilder {
  return {
    id: Number(message.capture_id ?? 0),
    trigger: String(message.trigger ?? ""),
    source: String(message.source ?? ""),
    edge: String(message.edge ?? ""),
    thresholdUa: Number(message.threshold_ua ?? 0),
    rateHz: Number(message.rate_hz ?? 0),
    preSamples: Number(message.pre_samples ?? 0),
    postSamples: Number(message.post_samples ?? 0),
    triggerOffset: Number(message.trigger_offset ?? 0),
    expected: Number(message.sample_count ?? 0),
    samples: [],
  };
}

export function appendLegacyCaptureSamples(
  builder: LegacyCaptureBuilder,
  frames: unknown[],
): void {
  for (const frame of frames) {
    if (!isRecord(frame)) continue;
    builder.samples.push({
      offset: Number(frame.offset ?? builder.samples.length),
      triggered: frame.triggered === true,
      sampleSequence: Number(frame.sample_sequence ?? 0),
      deviceTimeUs: Number(frame.device_t_mono_us ?? 0),
      readings: parseCaptureCurrentReadings(frame.readings),
    });
  }
}

export function completeLegacyCapture(
  builder: LegacyCaptureBuilder,
  capturedAt: number,
): PowerCapture {
  return {
    id: builder.id,
    trigger: builder.trigger,
    source: builder.source,
    edge: builder.edge,
    thresholdUa: builder.thresholdUa,
    rateHz: builder.rateHz,
    preSamples: builder.preSamples,
    postSamples: builder.postSamples,
    triggerOffset: builder.triggerOffset,
    samples: builder.samples,
    capturedAt,
  };
}

export function powerCaptureArmMessage(config: CaptureConfig) {
  return {
    type: "command",
    command: "capture_arm",
    id: "web-capture",
    mode: POWER_CAPTURE_PROTOCOL,
    trigger: config.trigger,
    output: config.trigger === "gpio" ? "" : config.source,
    gpio: config.trigger === "gpio" ? config.source : "",
    edge: config.edge,
    threshold_ua: config.thresholdUa,
    rate_hz: config.rateHz,
    // Streaming mode uses the capture engine only as a precise trigger
    // detector. The complete record flows through telemetry and is persisted
    // by the host, so it is not bounded by firmware capture RAM.
    // These fields are retained only so the Web UI can still arm older
    // firmware. Trigger-only firmware ignores them; the host owns buffering.
    pre_samples: 0,
    post_samples: 1,
  };
}
