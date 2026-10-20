import { act } from "react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  apiMocks, flushMicrotasks, flushNextFrame, renderBoard, renderLiveBoard,
  setupUseBoardHarness, teardownUseBoardHarness, telemetryMessage,
} from "./useBoard.testUtils";

describe("useBoard authoritative power state freshness", () => {
  beforeEach(() => {
    setupUseBoardHarness();
    vi.useFakeTimers({ toFake: ["setTimeout", "clearTimeout", "setInterval", "clearInterval", "Date"] });
  });
  afterEach(() => { vi.useRealTimers(); teardownUseBoardHarness(); });

  it("expires a paused snapshot and becomes writable only after another status update", async () => {
    const view = renderBoard();
    expect(view.current().powerStateStale).toBe(true);
    expect(view.current().powerUpdatedAt).toBeNull();
    await flushMicrotasks();
    expect(view.current().powerStateStale).toBe(false);
    expect(view.current().powerUpdatedAt).toBe(Date.now());
    act(() => view.current().setAuto(false));
    await flushMicrotasks();
    act(() => vi.advanceTimersByTime(29_999));
    expect(view.current().powerStateStale).toBe(false);
    act(() => vi.advanceTimersByTime(1));
    expect(view.current().connected).toBe(true);
    expect(view.current().powerStateStale).toBe(true);
    await act(async () => view.current().refresh());
    expect(view.current().powerStateStale).toBe(false);
    expect(view.current().powerUpdatedAt).toBe(Date.now());
    view.close();
  });

  it("does not let ADC traffic keep power state fresh after status updates stop", async () => {
    const { view, socket } = await renderLiveBoard();
    const originalPowerUpdatedAt = view.current().powerUpdatedAt;
    expect(originalPowerUpdatedAt).toBe(Date.now());
    apiMocks.getStatus.mockRejectedValue(new Error("status unavailable"));
    await act(async () => vi.advanceTimersByTimeAsync(29_000));
    act(() => socket.emitMessage(telemetryMessage(100, 1)));
    flushNextFrame();
    expect(view.current().lastVerifiedAt).toBe(Date.now());
    expect(view.current().powerUpdatedAt).toBe(originalPowerUpdatedAt);
    act(() => socket.emitMessage({ type: "snapshot", gpios: [] }));
    expect(view.current().powerUpdatedAt).toBe(originalPowerUpdatedAt);
    expect(view.current().powerStateStale).toBe(false);
    await act(async () => vi.advanceTimersByTimeAsync(1000));
    expect(view.current().connected).toBe(true);
    expect(view.current().powerStateStale).toBe(true);
    act(() => socket.emitMessage({ type: "snapshot", power_outputs: [{ name: "vdd_5v", state: "off", value: 0 }] }));
    expect(view.current().powerStateStale).toBe(false);
    expect(view.current().powerUpdatedAt).toBe(Date.now());
    view.close();
  });
});
