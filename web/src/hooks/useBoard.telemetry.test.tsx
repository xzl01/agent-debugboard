import { act } from "react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import {
  armStreamingCapture,
  emitCaptureTriggered,
} from "./useBoard.fixtures";
import {
  currentReading,
  flushNextFrame,
  renderLiveBoard,
  scheduledFrameCount,
  setupUseBoardHarness,
  teardownUseBoardHarness,
  telemetryBatchMessage,
} from "./useBoard.testUtils";

describe("useBoard telemetry decoding", () => {
  beforeEach(setupUseBoardHarness);
  afterEach(teardownUseBoardHarness);

  it("publishes only the latest batch sample as the preview", async () => {
    // Given an open live session
    const { view, socket } = await renderLiveBoard();

    // When a telemetry batch arrives
    act(() => {
      socket.emitMessage(telemetryBatchMessage([1, 2, 3]));
    });

    // Then one animation frame is scheduled and nothing is published yet
    expect(scheduledFrameCount()).toBe(1);
    expect(view.current().snapshot.adc).toEqual([]);

    // When the frame fires
    flushNextFrame();

    // Then only the latest sample's readings are published
    const adc = view.current().snapshot.adc;
    expect(adc).toHaveLength(1);
    expect(adc[0]?.value).toBe(300);
    view.close();
  });

  it("ingests every batch sample into a recording capture while visible", async () => {
    // Given a recording streaming capture
    const { view, socket } = await renderLiveBoard();
    await armStreamingCapture(view, socket);
    emitCaptureTriggered(socket);

    // When a telemetry batch arrives
    act(() => {
      socket.emitMessage(telemetryBatchMessage([1, 2, 3, 4, 5]));
    });

    // Then every sample is ingested immediately
    expect(view.current().captureProgress?.received).toBe(5);

    // And the foreground preview still publishes the latest reading
    flushNextFrame();
    expect(view.current().snapshot.adc[0]?.value).toBe(500);
    view.close();
  });

  it("falls back to sequence and uptime_us when monotonic fields are missing", async () => {
    // Given a recording streaming capture
    const { view, socket } = await renderLiveBoard();
    await armStreamingCapture(view, socket);
    emitCaptureTriggered(socket);

    // When a telemetry frame uses only the legacy field names
    act(() => {
      socket.emitMessage({
        type: "telemetry",
        readings: [currentReading(42)],
        sequence: 9,
        uptime_us: 12345,
      });
    });

    // Then the sample is still ingested and previewed
    expect(view.current().captureProgress?.received).toBe(1);
    flushNextFrame();
    expect(view.current().snapshot.adc[0]?.value).toBe(42);
    view.close();
  });

  it("ignores malformed frames without scheduling a preview or raising an error", async () => {
    // Given an open live session
    const { view, socket } = await renderLiveBoard();

    // When malformed and unknown frames arrive
    act(() => {
      socket.onmessage?.({ data: "not json" });
      socket.onmessage?.({ data: "null" });
      socket.onmessage?.({ data: JSON.stringify([1, 2, 3]) });
      socket.onmessage?.({ data: JSON.stringify({ type: "unknown" }) });
    });

    // Then nothing changes and no preview is scheduled
    expect(view.current().snapshot.adc).toEqual([]);
    expect(scheduledFrameCount()).toBe(0);
    expect(view.current().error).toBeNull();
    view.close();
  });
});
