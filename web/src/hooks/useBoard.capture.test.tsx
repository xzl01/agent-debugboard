import { act } from "react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import {
  liveSubscribeMessage,
  TELEMETRY_STREAM_BATCH_SIZE,
} from "@/lib/liveSubscribe";
import type { CaptureConfig } from "@/lib/types";
import {
  flushMicrotasks,
  flushNextFrame,
  renderLiveBoard,
  scheduledFrameCount,
  setHidden,
  setupUseBoardHarness,
  teardownUseBoardHarness,
  telemetryBatchMessage,
  telemetryMessage,
} from "./useBoard.testUtils";

describe("useBoard hidden capture fidelity", () => {
  beforeEach(setupUseBoardHarness);
  afterEach(teardownUseBoardHarness);

  it("keeps hidden capture ingestion connected and lossless while suppressing the preview", async () => {
    // Given an armed streaming capture on an open live session
    const { view, socket } = await renderLiveBoard();
    const config: CaptureConfig = {
      trigger: "manual",
      source: "5v_out",
      edge: "rising",
      thresholdUa: 0,
      rateHz: 500,
      preSamples: 0,
      postSamples: 0,
      streaming: true,
    };
    let armPromiseBox: Promise<void> | null = null;
    act(() => {
      armPromiseBox = view.current().armCapture(config);
    });
    if (!armPromiseBox) throw new Error("armCapture did not start");
    const armPromise: Promise<void> = armPromiseBox;
    await flushMicrotasks();
    expect(JSON.parse(socket.sent[1] ?? "")).toEqual(
      liveSubscribeMessage(500, TELEMETRY_STREAM_BATCH_SIZE),
    );
    act(() => {
      socket.emitMessage({ type: "result", command: "capture_arm", id: "web-capture" });
    });
    await armPromise;
    await flushMicrotasks();
    act(() => {
      socket.emitMessage({
        type: "capture_triggered",
        capture_id: 7,
        sample_sequence: 1,
        device_t_mono_us: 2000,
        dropped_samples: 0,
      });
    });
    expect(view.current().captureState).toBe("recording");

    // When the tab is hidden and a telemetry batch arrives
    setHidden(true);
    act(() => {
      socket.emitMessage(telemetryBatchMessage([1, 2, 3, 4, 5]));
    });

    // Then every sample is ingested while the preview stays silent and unscheduled
    expect(view.current().captureProgress?.received).toBe(5);
    expect(view.current().snapshot.adc).toEqual([]);
    expect(scheduledFrameCount()).toBe(0);

    // When the tab becomes visible again and new telemetry arrives
    setHidden(false);
    act(() => {
      socket.emitMessage(telemetryMessage(900, 6));
    });
    flushNextFrame();

    // Then the preview publishes the latest reading again
    expect(view.current().snapshot.adc[0]?.value).toBe(900);
    view.close();
  });
});
