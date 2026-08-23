import { act } from "react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import {
  armStreamingCapture,
  streamingCaptureConfig,
} from "./useBoard.fixtures";
import {
  apiMocks,
  flushMicrotasks,
  powerCaptureStoreMocks,
  renderLiveBoard,
  setupUseBoardHarness,
  teardownUseBoardHarness,
} from "./useBoard.testUtils";

describe("useBoard capture arm and validation", () => {
  beforeEach(setupUseBoardHarness);
  afterEach(teardownUseBoardHarness);

  it("resolves the arm promise and enters armed state on the capture_arm result", async () => {
    // Given an open live session
    const { view, socket } = await renderLiveBoard();

    // When a streaming capture is armed
    await armStreamingCapture(view, socket);

    // Then the arm promise resolved and the state is armed
    expect(view.current().captureState).toBe("armed");
    expect(view.current().error).toBeNull();
    view.close();
  });

  it("rejects a superseded arm with the existing error", async () => {
    // Given a first arm in flight
    const { view, socket } = await renderLiveBoard();
    let first: Promise<void> | null = null;
    act(() => {
      first = view.current().armCapture(streamingCaptureConfig());
    });
    if (!first) throw new Error("first arm did not start");
    const firstRejection = expect(first).rejects.toThrow(
      "Power capture arming was superseded",
    );
    await flushMicrotasks();

    // When a second arm supersedes it
    let second: Promise<void> | null = null;
    act(() => {
      second = view.current().armCapture(streamingCaptureConfig());
    });
    if (!second) throw new Error("second arm did not start");
    const secondArm: Promise<void> = second;

    // Then the first arm rejects and the second can complete
    await firstRejection;
    await flushMicrotasks();
    act(() => {
      socket.emitMessage({ type: "result", command: "capture_arm", id: "web-capture" });
    });
    await secondArm;
    expect(view.current().captureState).toBe("armed");
    view.close();
  });

  it("rejects before any wire command when storage capacity is insufficient", async () => {
    // Given storage capacity validation fails
    powerCaptureStoreMocks.ensurePowerCaptureStorageCapacity.mockRejectedValue(
      new Error("storage is full"),
    );
    const { view, socket } = await renderLiveBoard();
    const sentBefore = socket.sent.length;

    // When a streaming capture is armed
    let arm: Promise<void> | null = null;
    act(() => {
      arm = view.current().armCapture(streamingCaptureConfig());
    });
    if (!arm) throw new Error("arm did not start");

    // Then the arm rejects without touching the wire
    await expect(arm).rejects.toThrow("storage is full");
    await flushMicrotasks();
    expect(socket.sent).toHaveLength(sentBefore);
    expect(view.current().captureState).toBe("idle");
    expect(view.current().error).toBe("storage is full");
    view.close();
  });

  it("rejects streaming arms that violate protocol, rate, or pre-trigger limits", async () => {
    // Given firmware without the streaming protocol
    apiMocks.getStatus.mockResolvedValue({
      power_outputs: [], gpios: [], switches: {}, watchdog: {}, board_monitoring: {},
    });
    const { view } = await renderLiveBoard();

    // When a streaming capture is armed against the old protocol
    let arm: Promise<void> | null = null;
    act(() => {
      arm = view.current().armCapture(streamingCaptureConfig());
    });
    if (!arm) throw new Error("arm did not start");

    // Then the exact protocol error is raised
    await expect(arm).rejects.toThrow(
      "Power capture requires firmware protocol host-stream-v1; the debugger reported not reported",
    );
    view.close();
  });

  it("rejects streaming arms above the rate and pre-trigger limits", async () => {
    // Given a live session with the streaming protocol
    const { view } = await renderLiveBoard();

    // When the rate exceeds the Web streaming limit
    let fastArm: Promise<void> | null = null;
    act(() => {
      fastArm = view.current().armCapture(streamingCaptureConfig({ rateHz: 501 }));
    });
    if (!fastArm) throw new Error("fast arm did not start");
    await expect(fastArm).rejects.toThrow(
      "Continuous Web recording is limited to 500 Hz to keep USB telemetry and the control API responsive",
    );

    // When the pre-trigger history exceeds the browser memory limit
    let deepArm: Promise<void> | null = null;
    act(() => {
      deepArm = view.current().armCapture(streamingCaptureConfig({ preSamples: 60001 }));
    });
    if (!deepArm) throw new Error("deep arm did not start");
    await expect(deepArm).rejects.toThrow(
      "Pre-trigger history is limited to 60000 samples to protect browser memory",
    );
    view.close();
  });
});
