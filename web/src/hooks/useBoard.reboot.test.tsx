import { act } from "react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { apiMocks, flushMicrotasks, renderBoard, renderLiveBoard, setupUseBoardHarness, teardownUseBoardHarness } from "./useBoard.testUtils";
import { REBOOT_RECONNECT_TIMEOUT_MS, REBOOT_REQUEST_TIMEOUT_MS } from "./useBoardReboot";

describe("useBoard normal restart lifecycle", () => {
  beforeEach(() => { setupUseBoardHarness(); vi.useFakeTimers(); });
  afterEach(() => { vi.useRealTimers(); teardownUseBoardHarness(); });
  const advance = async (ms: number) => {
    await act(async () => { await vi.advanceTimersByTimeAsync(ms); });
    await flushMicrotasks();
  };
  const statusWithUptime = (uptime_ms: number) => ({
    board_monitoring: { runtime: { available: true, uptime_ms } },
  });

  it("finishes after accepted reboot when uptime resets from 100000 to 500 without any sampled outage", async () => {
    apiMocks.getStatus.mockResolvedValue(statusWithUptime(100_000));
    const view = renderBoard();
    await flushMicrotasks();
    const before = apiMocks.getStatus.mock.calls.length;
    await act(async () => view.current().rebootBoard());
    expect(view.current().restarting).toBe(true);
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(before);
    apiMocks.getStatus.mockResolvedValue(statusWithUptime(500));
    await advance(2000);
    expect(view.current().connected).toBe(true);
    expect(view.current().restarting).toBe(false);
    expect(view.current().restartTimedOut).toBe(false);
    expect(view.current().error).toBeNull();
    await advance(REBOOT_RECONNECT_TIMEOUT_MS);
    expect(view.current().restartTimedOut).toBe(false);
    expect(apiMocks.rebootBoard).toHaveBeenCalledTimes(1);
    view.close();
  });

  it("does not finish on an increasing same-boot uptime or ordinary backward status jitter", async () => {
    apiMocks.getStatus.mockResolvedValue(statusWithUptime(100_000));
    const view = renderBoard();
    await flushMicrotasks();
    await act(async () => view.current().rebootBoard());
    for (const uptime of [102_000, 104_000, 103_000, 99_000, 90_000, 40_000]) {
      apiMocks.getStatus.mockResolvedValue(statusWithUptime(uptime));
      await advance(2000);
      expect(view.current().connected).toBe(true);
      expect(view.current().restarting).toBe(true);
    }
    view.close();
  });

  it.each([undefined, NaN, Infinity, -1])("does not invent a reset from unknown/invalid baseline %s", async (uptime) => {
    apiMocks.getStatus.mockResolvedValue({ board_monitoring: { runtime: { available: true, uptime_ms: uptime } } });
    const view = renderBoard();
    await flushMicrotasks();
    await act(async () => view.current().rebootBoard());
    apiMocks.getStatus.mockResolvedValue(statusWithUptime(500));
    await advance(2000);
    expect(view.current().restarting).toBe(true);
    expect(view.current().restartTimedOut).toBe(false);
    view.close();
  });

  it("uses available seconds-only runtime metadata but never unavailable uptime", async () => {
    apiMocks.getStatus.mockResolvedValue({ board_monitoring: { runtime: { available: true, uptime_seconds: 100 } } });
    const view = renderBoard();
    await flushMicrotasks();
    await act(async () => view.current().rebootBoard());
    apiMocks.getStatus.mockResolvedValue({ board_monitoring: { runtime: { available: false, uptime_ms: 500 } } });
    await advance(2000);
    expect(view.current().restarting).toBe(true);
    apiMocks.getStatus.mockResolvedValue(statusWithUptime(500));
    await advance(2000);
    expect(view.current().restarting).toBe(false);
    view.close();
  });

  it("does not claim reboot completion before the POST acknowledgement even if uptime already rolled back", async () => {
    let accept!: () => void;
    apiMocks.rebootBoard.mockImplementationOnce(() => new Promise<void>((resolve) => { accept = resolve; }));
    apiMocks.getStatus.mockResolvedValue(statusWithUptime(100_000));
    const view = renderBoard();
    await flushMicrotasks();
    let request!: Promise<void>;
    act(() => { request = view.current().rebootBoard(); });
    apiMocks.getStatus.mockResolvedValue(statusWithUptime(500));
    await advance(2000);
    expect(view.current().restarting).toBe(true);
    await act(async () => { accept(); await request; });
    expect(view.current().restarting).toBe(false);
    view.close();
  });

  it("accepts the POST once without immediate refresh, then treats the HTTP disconnect and reconnect as expected", async () => {
    const view = renderBoard();
    await flushMicrotasks();
    const statusCalls = apiMocks.getStatus.mock.calls.length;
    const adcCalls = apiMocks.getAdc.mock.calls.length;
    await act(async () => view.current().rebootBoard());
    expect(apiMocks.rebootBoard).toHaveBeenCalledTimes(1);
    expect(apiMocks.enterBootloader).not.toHaveBeenCalled();
    expect(view.current().restarting).toBe(true);
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(statusCalls);
    expect(apiMocks.getAdc).toHaveBeenCalledTimes(adcCalls);
    apiMocks.getStatus.mockRejectedValueOnce(new Error("Expected network disconnect"));
    await advance(2000);
    expect(view.current().connected).toBe(false);
    expect(view.current().error).toBeNull();
    expect(view.current().restarting).toBe(true);
    await advance(2000);
    expect(view.current().connected).toBe(true);
    expect(view.current().error).toBeNull();
    expect(view.current().restarting).toBe(false);
    expect(view.current().restartTimedOut).toBe(false);
    expect(apiMocks.rebootBoard).toHaveBeenCalledTimes(1);
    view.close();
  });

  it("does not surface the expected live WebSocket error/close and lets existing fallback polling reconnect", async () => {
    const { view, socket } = await renderLiveBoard();
    const before = apiMocks.getStatus.mock.calls.length;
    await act(async () => view.current().rebootBoard());
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(before);
    apiMocks.getStatus.mockRejectedValueOnce(new Error("Expected restart outage"));
    act(() => { socket.onerror?.(); socket.emitClose(); });
    await flushMicrotasks();
    expect(view.current().live).toBe(false);
    expect(view.current().connected).toBe(false);
    expect(view.current().error).toBeNull();
    expect(view.current().restarting).toBe(true);
    await advance(2000);
    expect(view.current().connected).toBe(true);
    expect(view.current().restarting).toBe(false);
    expect(view.current().error).toBeNull();
    view.close();
  });

  it("rejects concurrent restart attempts before issuing a second POST", async () => {
    let accept!: () => void;
    apiMocks.rebootBoard.mockImplementation(() => new Promise<void>((resolve) => { accept = resolve; }));
    const view = renderBoard();
    await flushMicrotasks();
    let request!: Promise<void>;
    act(() => { request = view.current().rebootBoard(); });
    await act(async () => { await expect(view.current().rebootBoard()).rejects.toThrow("already in progress"); });
    expect(apiMocks.rebootBoard).toHaveBeenCalledTimes(1);
    await act(async () => { accept(); await request; });
    view.close();
  });

  it("bounds a hung request, aborts it and allows a subsequent explicit attempt", async () => {
    apiMocks.rebootBoard.mockImplementationOnce(() => new Promise<void>(() => {}));
    const view = renderBoard();
    await flushMicrotasks();
    let request!: Promise<void>;
    act(() => { request = view.current().rebootBoard(); });
    const rejected = expect(request).rejects.toMatchObject({ code: "reboot_request_timeout" });
    const signal = apiMocks.rebootBoard.mock.calls[0]![0]!;
    await advance(REBOOT_REQUEST_TIMEOUT_MS);
    await rejected;
    expect(signal.aborted).toBe(true);
    expect(view.current().restarting).toBe(false);
    await act(async () => view.current().rebootBoard());
    expect(apiMocks.rebootBoard).toHaveBeenCalledTimes(2);
    view.close();
  });

  it("expires an accepted restart window without retrying and restores ordinary disconnect errors", async () => {
    let uptime = 98_000;
    apiMocks.getStatus.mockImplementation(async () => statusWithUptime(uptime += 2000));
    const view = renderBoard();
    await flushMicrotasks();
    await act(async () => view.current().rebootBoard());
    await advance(REBOOT_RECONNECT_TIMEOUT_MS);
    expect(view.current().restarting).toBe(false);
    expect(view.current().restartTimedOut).toBe(true);
    expect(view.current().connected).toBe(true);
    expect(apiMocks.rebootBoard).toHaveBeenCalledTimes(1);
    apiMocks.getStatus.mockRejectedValueOnce(new Error("Still offline after the restart window"));
    await advance(2000);
    expect(view.current().error).toBe("Still offline after the restart window");
    expect(view.current().connected).toBe(false);
    view.close();
  });

  it("does not suppress ordinary connection errors when the reboot POST is refused", async () => {
    apiMocks.rebootBoard.mockRejectedValueOnce(new Error("Hardware task busy"));
    const view = renderBoard();
    await flushMicrotasks();
    await act(async () => { await expect(view.current().rebootBoard()).rejects.toThrow("Hardware task busy"); });
    expect(view.current().restarting).toBe(false);
    apiMocks.getStatus.mockRejectedValueOnce(new Error("Ordinary outage"));
    await advance(2000);
    expect(view.current().error).toBe("Ordinary outage");
    view.close();
  });
});
