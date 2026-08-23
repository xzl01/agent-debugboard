import { act } from "react";
import type { CaptureConfig, PowerCapture } from "@/lib/types";
import type { PowerCaptureArchiveInfo } from "@/lib/powerCaptureStore";
import {
  flushMicrotasks,
  type BoardView,
  type MockWebSocket,
} from "./useBoard.testUtils";

export function streamingCaptureConfig(overrides: Partial<CaptureConfig> = {}): CaptureConfig {
  return {
    trigger: "manual",
    source: "5v_out",
    edge: "rising",
    thresholdUa: 0,
    rateHz: 500,
    preSamples: 0,
    postSamples: 0,
    streaming: true,
    ...overrides,
  };
}

export function powerCaptureArchiveInfo(capture: PowerCapture): PowerCaptureArchiveInfo {
  return {
    archiveId: capture.archiveId ?? "archive",
    status: "complete",
    chunkCount: 0,
    persistedSamples: capture.sampleCount ?? capture.samples.length,
    estimatedBytes: 0,
    updatedAt: 0,
    pinned: false,
    capture,
  };
}

export async function armStreamingCapture(
  view: BoardView,
  socket: MockWebSocket,
  config: CaptureConfig = streamingCaptureConfig(),
): Promise<void> {
  let armPromise: Promise<void> | null = null;
  act(() => {
    armPromise = view.current().armCapture(config);
  });
  if (!armPromise) throw new Error("armCapture did not start");
  await flushMicrotasks();
  act(() => {
    socket.emitMessage({ type: "result", command: "capture_arm", id: "web-capture" });
  });
  await armPromise;
  await flushMicrotasks();
}

export function emitCaptureTriggered(
  socket: MockWebSocket,
  overrides: Record<string, unknown> = {},
): void {
  act(() => {
    socket.emitMessage({
      type: "capture_triggered",
      capture_id: 7,
      sample_sequence: 1,
      device_t_mono_us: 2000,
      dropped_samples: 0,
      ...overrides,
    });
  });
}

export function sentCommands(socket: MockWebSocket): Record<string, unknown>[] {
  return socket.sent.map(
    (payload): Record<string, unknown> => JSON.parse(payload),
  );
}

export async function stopAndAcknowledgeCapture(
  view: BoardView,
  socket: MockWebSocket,
  captureId = 7,
): Promise<void> {
  act(() => {
    view.current().stopCapture();
  });
  await flushMicrotasks();
  act(() => {
    socket.emitMessage({
      type: "result",
      command: "capture_stop",
      id: `web-stop-${captureId}`,
    });
  });
  await flushMicrotasks();
}
