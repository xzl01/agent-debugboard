import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  MockWebSocket,
  renderLiveBoard,
  setupUseBoardHarness,
  teardownUseBoardHarness,
} from "./useBoard.testUtils";
import { useBoardLive } from "./useBoardLive";

const boardTelemetryMocks = vi.hoisted(() => ({
  decodeTelemetryReadings: vi.fn(),
  decodeTelemetrySamples: vi.fn(),
}));

vi.mock("@/lib/boardTelemetry", () => boardTelemetryMocks);

let socketFailureSpy = vi.fn<(message: string) => void>();

function LiveProbe() {
  const handleSocketFailure = socketFailureSpy;
  useBoardLive({
    live: true,
    pageVisible: true,
    auto: false,
    refresh: vi.fn(),
    wsRef: { current: null },
    pendingCaptureRef: { current: null },
    setSnapshot: vi.fn(),
    setHasData: vi.fn(),
    setConnected: vi.fn(),
    setError: vi.fn(),
    setLoading: vi.fn(),
    setLive: vi.fn(),
    handleCaptureMessage: vi.fn(),
    handleSocketFailure,
    handleSessionCleanup: vi.fn(),
  });
  return null;
}

function renderLiveProbe(): Root {
  const host = document.createElement("div");
  const root = createRoot(host);
  act(() => root.render(<LiveProbe />));
  return root;
}

describe("useBoardLive dispatch failures", () => {
  beforeEach(() => {
    setupUseBoardHarness();
    socketFailureSpy.mockClear();
    boardTelemetryMocks.decodeTelemetryReadings.mockReturnValue([]);
    boardTelemetryMocks.decodeTelemetrySamples.mockReturnValue([]);
  });

  afterEach(teardownUseBoardHarness);

  it("routes non-parse telemetry dispatch failures through handleSocketFailure", async () => {
    boardTelemetryMocks.decodeTelemetrySamples.mockImplementation(() => {
      throw new Error("boom");
    });
    const { view, socket } = await renderLiveBoard();

    act(() => {
      socket.emitMessage({ type: "telemetry", topic: "adc", readings: [] });
    });

    expect(boardTelemetryMocks.decodeTelemetrySamples).toHaveBeenCalledTimes(1);
    expect(view.current().connected).toBe(false);
    view.close();
  });

  it("does not route dispatch failures after the effect is cleaned up", async () => {
    boardTelemetryMocks.decodeTelemetrySamples.mockImplementation(() => {
      throw new Error("boom");
    });
    const root = renderLiveProbe();
    await act(async () => {
      await Promise.resolve();
      await Promise.resolve();
    });
    const socket = MockWebSocket.instances.at(-1);
    if (!socket) throw new Error("live WebSocket was not created");
    act(() => socket.emitOpen());

    act(() => root.unmount());
    act(() => {
      socket.onmessage?.({
        data: JSON.stringify({ type: "telemetry", topic: "adc", readings: [] }),
      });
    });

    expect(socketFailureSpy).not.toHaveBeenCalled();
  });
});
