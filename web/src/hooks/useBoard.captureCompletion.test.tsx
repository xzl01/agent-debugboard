import { act } from "react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import type { CaptureConfig } from "@/lib/types";
import {
  armStreamingCapture,
  emitCaptureTriggered,
  powerCaptureArchiveInfo,
  sentCommands,
  stopAndAcknowledgeCapture,
  streamingCaptureConfig,
} from "./useBoard.fixtures";
import {
  flushMicrotasks,
  powerCaptureStoreMocks,
  renderLiveBoard,
  setupUseBoardHarness,
  teardownUseBoardHarness,
  telemetryBatchMessage,
} from "./useBoard.testUtils";

describe("useBoard capture completion", () => {
  beforeEach(setupUseBoardHarness);
  afterEach(teardownUseBoardHarness);

  it("splits the pre-trigger buffer around the trigger sample", async () => {
    // Given an armed capture with a two-sample pre-trigger history
    const finishMock = powerCaptureStoreMocks.finishPowerCaptureArchive;
    finishMock.mockImplementation(
      async (capture) => powerCaptureArchiveInfo(capture),
    );
    const { view, socket } = await renderLiveBoard();
    await armStreamingCapture(view, socket, streamingCaptureConfig({ preSamples: 2 }));
    act(() => {
      socket.emitMessage(telemetryBatchMessage([1, 2, 3]));
    });

    // When the trigger fires at sequence 2 and the capture is stopped
    emitCaptureTriggered(socket, { sample_sequence: 2, device_t_mono_us: 4000 });
    await stopAndAcknowledgeCapture(view, socket);

    // Then the record keeps one pre-trigger sample and one post-trigger sample
    expect(finishMock).toHaveBeenCalledTimes(1);
    const record = finishMock.mock.calls[0]?.[0];
    expect(record?.preSamples).toBe(1);
    expect(record?.postSamples).toBe(1);
    expect(record?.sampleCount).toBe(3);
    expect(record?.triggerOffset).toBe(1);
    expect(record?.samples.map((sample) => sample.sampleSequence)).toEqual([1, 2, 3]);
    view.close();
  });

  it("waits for the capture_stop acknowledgement before sealing the archive", async () => {
    // Given a recording capture
    const finishMock = powerCaptureStoreMocks.finishPowerCaptureArchive;
    finishMock.mockImplementation(
      async (capture) => powerCaptureArchiveInfo(capture),
    );
    const { view, socket } = await renderLiveBoard();
    await armStreamingCapture(view, socket);
    emitCaptureTriggered(socket);
    act(() => {
      socket.emitMessage(telemetryBatchMessage([1, 2]));
    });

    // When the user stops the capture
    act(() => {
      view.current().stopCapture();
    });
    await flushMicrotasks();

    // Then the stop command is on the wire but the archive is not sealed yet
    expect(view.current().captureState).toBe("receiving");
    expect(sentCommands(socket).some((cmd) =>
      cmd.command === "capture_stop" && cmd.id === "web-stop-7"
    )).toBe(true);
    expect(finishMock).not.toHaveBeenCalled();

    // When the acknowledgement arrives
    act(() => {
      socket.emitMessage({ type: "result", command: "capture_stop", id: "web-stop-7" });
    });
    await flushMicrotasks();

    // Then the archive is sealed and the capture is complete
    expect(finishMock).toHaveBeenCalledTimes(1);
    expect(view.current().captureState).toBe("idle");
    expect(view.current().captures.at(-1)?.incomplete).toBe(false);
    view.close();
  });

  it("completes the capture as interrupted when capture_stop fails", async () => {
    // Given a recording capture being stopped
    const finishMock = powerCaptureStoreMocks.finishPowerCaptureArchive;
    finishMock.mockImplementation(
      async (capture) => powerCaptureArchiveInfo(capture),
    );
    const { view, socket } = await renderLiveBoard();
    await armStreamingCapture(view, socket);
    emitCaptureTriggered(socket);
    act(() => {
      view.current().stopCapture();
    });
    await flushMicrotasks();

    // When the debugger reports a stop failure
    act(() => {
      socket.emitMessage({ type: "error", command: "capture_stop", error: { message: "boom" } });
    });
    await flushMicrotasks();

    // Then the capture completes as interrupted with the reported reason
    expect(finishMock).toHaveBeenCalledWith(
      expect.objectContaining({ incomplete: true, interruptionReason: "boom" }),
      0,
    );
    expect(view.current().error).toBe("boom");
    expect(view.current().captureState).toBe("idle");
    view.close();
  });

  it("finalizes a triggered capture as interrupted when the socket closes", async () => {
    // Given a recording capture
    const finishMock = powerCaptureStoreMocks.finishPowerCaptureArchive;
    finishMock.mockImplementation(
      async (capture) => powerCaptureArchiveInfo(capture),
    );
    const { view, socket } = await renderLiveBoard();
    await armStreamingCapture(view, socket);
    emitCaptureTriggered(socket);

    // When the live socket closes
    act(() => {
      socket.emitClose();
    });

    // Then the disconnect is reported and live mode is left
    expect(view.current().live).toBe(false);
    expect(view.current().error).toBe("Live WebSocket disconnected");

    // When the archive finalization settles
    await flushMicrotasks();

    // Then the capture completes as interrupted and the state is idle
    expect(finishMock).toHaveBeenCalledWith(
      expect.objectContaining({
        incomplete: true,
        interruptionReason: "Live WebSocket disconnected",
      }),
      0,
    );
    expect(view.current().captureState).toBe("idle");
    view.close();
  });

  it("resets an armed but untriggered capture when the socket closes", async () => {
    // Given an armed capture that never triggered
    const finishMock = powerCaptureStoreMocks.finishPowerCaptureArchive;
    const { view, socket } = await renderLiveBoard();
    await armStreamingCapture(view, socket);

    // When the live socket closes
    act(() => {
      socket.emitClose();
    });

    // Then the capture is reset and the disconnect is reported
    expect(view.current().captureState).toBe("idle");
    expect(view.current().live).toBe(false);
    expect(view.current().error).toBe("Live WebSocket disconnected");

    // When the close settles
    await flushMicrotasks();

    // Then no archive was sealed
    expect(finishMock).not.toHaveBeenCalled();
    view.close();
  });

  it("assembles a legacy bounded capture from the capture_* message flow", async () => {
    // Given an armed legacy capture
    const { view, socket } = await renderLiveBoard();
    const legacyConfig: CaptureConfig = {
      trigger: "manual", source: "5v_out", edge: "rising", thresholdUa: 0,
      rateHz: 100, preSamples: 1, postSamples: 2,
    };
    let arm: Promise<void> | null = null;
    act(() => {
      arm = view.current().armCapture(legacyConfig);
    });
    if (!arm) throw new Error("arm did not start");
    const armPromise: Promise<void> = arm;
    await flushMicrotasks();
    act(() => {
      socket.emitMessage({ type: "result", command: "capture_arm", id: "web-capture" });
    });
    await armPromise;

    // When the legacy capture stream completes
    act(() => {
      socket.emitMessage({
        type: "capture_begin", capture_id: 9, trigger: "manual", source: "5v_out",
        edge: "rising", threshold_ua: 0, rate_hz: 100,
        pre_samples: 1, post_samples: 2, trigger_offset: 1, sample_count: 3,
      });
    });
    expect(view.current().captureState).toBe("receiving");
    act(() => {
      socket.emitMessage({
        type: "capture_samples",
        samples: [
          { offset: 0, triggered: false, sample_sequence: 1, device_t_mono_us: 100,
            readings: [{ name: "5v_out", power_enabled: true, current_ua: 1000 }] },
          { offset: 1, triggered: true, sample_sequence: 2, device_t_mono_us: 200,
            readings: [{ name: "5v_out", power_enabled: true, current_ua: 2000 }] },
          { offset: 2, triggered: false, sample_sequence: 3, device_t_mono_us: 300,
            readings: [{ name: "5v_out", power_enabled: true, current_ua: 1500 }] },
        ],
      });
    });
    expect(view.current().captureProgress).toEqual({ received: 3, total: 3 });
    act(() => {
      socket.emitMessage({ type: "capture_complete", capture_id: 9 });
    });

    // Then the completed capture is appended and the state is idle
    const capture = view.current().captures.at(-1);
    expect(capture).toMatchObject({
      id: 9, trigger: "manual", source: "5v_out", edge: "rising",
      thresholdUa: 0, rateHz: 100, preSamples: 1, postSamples: 2, triggerOffset: 1,
    });
    expect(capture?.samples).toHaveLength(3);
    expect(capture?.samples[1]?.triggered).toBe(true);
    expect(view.current().captureState).toBe("idle");
    expect(view.current().captureProgress).toBeNull();
    view.close();
  });
});
