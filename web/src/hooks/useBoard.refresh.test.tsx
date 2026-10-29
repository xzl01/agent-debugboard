import { act } from "react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { EMPTY_PD_STATUS } from "@/lib/pd";
import {
  apiMocks,
  flushMicrotasks,
  MockWebSocket,
  renderBoard,
  renderLiveBoard,
  setHidden,
  setupUseBoardHarness,
  teardownUseBoardHarness,
  type BoardView,
} from "./useBoard.testUtils";

const realApi = await vi.importActual<typeof import("@/lib/api")>("@/lib/api");

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (reason: Error) => void;
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}

function status(value: 0 | 1) {
  return {
    power_capture_protocol: "host-stream-v1",
    power_outputs: [{ name: "5v_out", state: value ? "on" : "off", value }],
    gpios: [],
  };
}

function adc(value: number) {
  return {
    readings: [{
      name: "5v_out", signal: "S_C_5V", sensor_channel: "current",
      current_ua: value, unit: "A", power_enabled: true,
    }],
  };
}

function httpStatus(value: 0 | 1) {
  return {
    ...status(value),
    mcu: "rp2350a",
    usb: "ncm-ecm",
    power_outputs: [{
      name: "5v_out", state: value ? "on" : "off", value,
      signal: "S_C_5V", gp: 7, controllable: false,
    }],
    pd: {
      ...EMPTY_PD_STATUS, available: true, pd: true,
      voltage_mv: 20_000, capabilities_valid: true,
    },
  };
}

// Firmware's status snapshot has state, but no PD or power pin metadata.
function liveStatus(value: 0 | 1) {
  return {
    type: "snapshot", topic: "status", sequence: 1,
    ...status(value),
    switches: { sd: { route: "target", routes: ["target", "usb-reader"], requires_confirm: false } },
    watchdog: { supported: true, automatic: true, healthy: true, armed: false, timeout_ms: 16_000 },
    gpio_layout_groups: [{ name: "J4", edge: "right", order: 0 }],
    gpios: [{ name: "GP12", pin: 12, direction: "input", value: 1 }],
    board_monitoring: { runtime: { available: true, uptime_ms: 200 } },
    config: { available: true, saved_count: 2, pending_count: 0 },
  };
}

function boardState(view: BoardView) {
  const board = view.current();
  return {
    snapshot: board.snapshot,
    connected: board.connected,
    powerUpdatedAt: board.powerUpdatedAt,
    lastVerifiedAt: board.lastVerifiedAt,
    hasData: board.hasData,
    error: board.error,
    loading: board.loading,
    live: board.live,
    gpioPhysicalLayout: board.gpioPhysicalLayout,
    gpioLayoutError: board.gpioLayoutError,
    gpioLayoutLoading: board.gpioLayoutLoading,
  };
}

function startRefresh(view: BoardView): Promise<void> {
  let request!: Promise<void>;
  act(() => { request = view.current().refresh(); });
  return request;
}

describe("useBoard refresh generations", () => {
  let views: BoardView[];
  const mount = (strictMode = false) => {
    const view = renderBoard({ strictMode });
    views.push(view);
    return view;
  };
  const mountLive = async () => {
    const result = await renderLiveBoard();
    views.push(result.view);
    return result;
  };

  beforeEach(() => {
    setupUseBoardHarness();
    vi.useFakeTimers({ toFake: ["setTimeout", "clearTimeout", "setInterval", "clearInterval", "Date"] });
    vi.setSystemTime(10_000);
    views = [];
  });
  afterEach(() => {
    for (const view of views) view.close();
    vi.restoreAllMocks();
    vi.useRealTimers();
    teardownUseBoardHarness();
  });

  it("keeps publishing slow automatic status/ADC reads across multiple polling periods", async () => {
    let reads = 0;
    let pending = 0;
    let maxPending = 0;
    const slow = (value: unknown, delay: number) => new Promise<unknown>((resolve) => {
      maxPending = Math.max(maxPending, ++pending);
      setTimeout(() => { --pending; resolve(value); }, delay);
    });
    apiMocks.getStatus.mockImplementation(() => {
      ++reads;
      return slow(status(reads % 2 ? 1 : 0), 1000);
    });
    apiMocks.getAdc.mockImplementation(() => slow(adc(reads * 100), 1500));
    const view = mount();
    await act(async () => vi.advanceTimersByTimeAsync(2000));
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(1);
    expect(view.current().loading).toBe(true);
    await act(async () => vi.advanceTimersByTimeAsync(500));
    for (let cycle = 1; cycle <= 4; ++cycle) {
      if (cycle > 1) await act(async () => vi.advanceTimersByTimeAsync(4500));
      expect(apiMocks.getStatus).toHaveBeenCalledTimes(cycle);
      expect(apiMocks.getAdc).toHaveBeenCalledTimes(cycle);
      expect(view.current().snapshot.adc[0]?.value).toBe(cycle * 100);
      expect(view.current().powerUpdatedAt).toBe(11_000 + (cycle - 1) * 4500);
      expect(view.current().loading).toBe(false);
      expect(view.current().hasData).toBe(true);
      expect(view.current().connected).toBe(true);
    }
    expect(maxPending).toBe(1);
  });

  it.each([
    ["status", "headers", false], ["status", "body", false],
    ["ADC", "headers", false], ["ADC", "body", false],
    ["status", "headers", true], ["status", "body", true],
    ["ADC", "headers", true], ["ADC", "body", true],
  ] as const)("resumes polling after hung %s %s without overwriting a manual success (StrictMode=%s)", async (phase, transport, strictMode) => {
    const hangingSignals: AbortSignal[] = [];
    const stalledPath = phase === "status" ? "/status" : "/adc/read";
    // A StrictMode status replay starts two reads; the superseded first status
    // never advances to ADC, so only one ADC read can be stalled.
    let stalls = phase === "status" && strictMode ? 2 : 1;
    const fetchMock = vi.fn<typeof fetch>(async (input, init) => {
      if (String(input).endsWith(stalledPath) && stalls > 0) {
        --stalls;
        hangingSignals.push(init!.signal!);
        if (transport === "headers") return new Promise(() => {});
        const accepted = new Response("{}");
        vi.spyOn(accepted, "text").mockImplementation(() => new Promise(() => {}));
        return accepted;
      }
      return new Response(JSON.stringify(String(input).endsWith("/status") ? httpStatus(1) : adc(200)));
    });
    vi.stubGlobal("fetch", fetchMock);
    apiMocks.getStatus.mockImplementation(realApi.getStatus);
    apiMocks.getAdc.mockImplementation(realApi.getAdc);
    const view = mount(strictMode);
    await flushMicrotasks();
    expect(hangingSignals).toHaveLength(phase === "status" && strictMode ? 2 : 1);

    await act(async () => vi.advanceTimersByTimeAsync(1000));
    await act(async () => { await view.current().refresh(); });
    await flushMicrotasks();
    const accepted = boardState(view);
    const calls = apiMocks.getStatus.mock.calls.length;
    expect(accepted.connected).toBe(true);
    expect(accepted.loading).toBe(false);
    expect(accepted.error).toBeNull();
    expect(accepted.snapshot.powerOutputs[0]?.value).toBe(1);
    expect(accepted.snapshot.adc[0]?.value).toBe(200);

    await act(async () => vi.advanceTimersByTimeAsync(realApi.BOARD_READ_TIMEOUT_MS - 1001));
    expect(hangingSignals.every((signal) => !signal.aborted)).toBe(true);
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(calls);
    await act(async () => vi.advanceTimersByTimeAsync(1));
    expect(hangingSignals.every((signal) => signal.aborted)).toBe(true);
    expect(boardState(view)).toEqual(accepted);
    expect(view.current().snapshot).toBe(accepted.snapshot);

    await act(async () => vi.advanceTimersByTimeAsync(1999));
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(calls);
    await act(async () => vi.advanceTimersByTimeAsync(1));
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(calls + 1);
    expect(view.current().powerUpdatedAt).toBe(10_000 + realApi.BOARD_READ_TIMEOUT_MS + 2000);

    await act(async () => vi.advanceTimersByTimeAsync(120_000));
    expect(apiMocks.getStatus.mock.calls.length).toBeGreaterThan(calls + 1);
    expect(view.current().connected).toBe(true);
    expect(view.current().loading).toBe(false);
    expect(view.current().error).toBeNull();
    expect(view.current().powerStateStale).toBe(false);
  });

  it.each(["status", "ADC"])("retries a current hung %s after its timeout and clears the error on recovery", async (phase) => {
    let signal: AbortSignal | null = null;
    vi.stubGlobal("fetch", vi.fn<typeof fetch>((_input, init) => {
      signal = init!.signal!;
      return new Promise(() => {});
    }));
    if (phase === "status") apiMocks.getStatus.mockImplementationOnce(realApi.getStatus);
    else apiMocks.getAdc.mockImplementationOnce(realApi.getAdc);
    const view = mount();
    await flushMicrotasks();
    await act(async () => vi.advanceTimersByTimeAsync(realApi.BOARD_READ_TIMEOUT_MS));
    expect(signal!.aborted).toBe(true);
    expect(view.current().connected).toBe(false);
    expect(view.current().loading).toBe(false);
    expect(view.current().error).toBe("Board API request timed out.");
    await act(async () => vi.advanceTimersByTimeAsync(2000));
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(2);
    expect(view.current().connected).toBe(true);
    expect(view.current().error).toBeNull();
  });

  it.each(["hidden", "unmount", "auto off", "live"])("invalidates a hung automatic request on %s without leaving a polling timer", async (change) => {
    const stalled = vi.fn<typeof fetch>(() => new Promise(() => {}));
    vi.stubGlobal("fetch", stalled);
    apiMocks.getStatus.mockImplementationOnce(realApi.getStatus);
    const view = mount();
    await flushMicrotasks();
    if (change === "hidden") setHidden(true);
    else if (change === "unmount") view.close();
    else act(() => change === "auto off" ? view.current().setAuto(false) : view.current().setLive(true));
    await flushMicrotasks();
    const accepted = boardState(view);
    const calls = apiMocks.getStatus.mock.calls.length;
    await act(async () => vi.advanceTimersByTimeAsync(realApi.BOARD_READ_TIMEOUT_MS));
    expect(boardState(view)).toEqual(accepted);
    expect(stalled.mock.calls[0][1]!.signal!.aborted).toBe(true);
    if (change === "live") {
      const socket = MockWebSocket.instances.at(-1)!;
      act(() => socket.emitOpen());
      view.close();
    }
    await act(async () => vi.advanceTimersByTimeAsync(120_000));
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(calls);
    expect(vi.getTimerCount()).toBe(0);
  });

  it.each(["seed status", "seed ADC", "status poll"])("aborts a live %s immediately on cleanup", async (phase) => {
    const fetchMock = vi.fn<typeof fetch>(() => new Promise(() => {}));
    vi.stubGlobal("fetch", fetchMock);
    const view = mount();
    await flushMicrotasks();
    if (phase === "seed ADC") apiMocks.getAdc.mockImplementationOnce(realApi.getAdc);
    else if (phase === "seed status") apiMocks.getStatus.mockImplementationOnce(realApi.getStatus);
    act(() => view.current().setLive(true));
    await flushMicrotasks();
    if (phase === "status poll") {
      const socket = MockWebSocket.instances.at(-1)!;
      act(() => socket.emitOpen());
      apiMocks.getStatus.mockImplementationOnce(realApi.getStatus);
      await act(async () => vi.advanceTimersByTimeAsync(10_000));
    }
    expect(fetchMock).toHaveBeenCalledOnce();
    const signal = fetchMock.mock.calls[0][1]!.signal!;
    expect(signal.aborted).toBe(false);
    view.close();
    expect(signal.aborted).toBe(true);
    await flushMicrotasks();
    expect(vi.getTimerCount()).toBe(0);
    const calls = apiMocks.getStatus.mock.calls.length;
    await act(async () => vi.advanceTimersByTimeAsync(120_000));
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(calls);
  });

  it.each(["success", "failure"])("isolates a StrictMode replay's old %s and polling cleanup", async (outcome) => {
    const oldStatus = deferred<unknown>();
    const newAdc = deferred<unknown>();
    apiMocks.getStatus.mockReturnValueOnce(oldStatus.promise).mockResolvedValueOnce(httpStatus(1));
    apiMocks.getAdc.mockReturnValueOnce(newAdc.promise);
    const view = mount(true);
    await flushMicrotasks();
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(2);
    expect(apiMocks.getAdc).toHaveBeenCalledTimes(1);
    const pending = boardState(view);
    await act(async () => {
      if (outcome === "success") oldStatus.resolve(httpStatus(0));
      else oldStatus.reject(new Error("discarded StrictMode session"));
    });
    expect(boardState(view)).toEqual(pending);
    expect(view.current().loading).toBe(true);
    expect(apiMocks.getGpioLayout).not.toHaveBeenCalled();

    await act(async () => newAdc.resolve(adc(200)));
    await flushMicrotasks();
    expect(view.current().snapshot.powerOutputs[0]?.value).toBe(1);
    expect(view.current().snapshot.adc[0]?.value).toBe(200);
    expect(view.current().loading).toBe(false);
    expect(apiMocks.getGpioLayout).toHaveBeenCalledTimes(1);
    await act(async () => vi.advanceTimersByTimeAsync(2000));
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(3);
  });

  it("keeps the newer status/ADC pair when two serial refreshes finish in reverse order", async () => {
    const oldAdc = deferred<unknown>();
    apiMocks.getStatus.mockResolvedValueOnce(status(0));
    apiMocks.getAdc.mockReturnValueOnce(oldAdc.promise);
    const view = mount();
    await flushMicrotasks();
    expect(apiMocks.getAdc).toHaveBeenCalledTimes(1);
    expect(apiMocks.getGpioLayout).not.toHaveBeenCalled();

    vi.setSystemTime(11_000);
    apiMocks.getStatus.mockResolvedValueOnce(status(1));
    apiMocks.getAdc.mockResolvedValueOnce(adc(200));
    await act(async () => { await view.current().refresh(); });
    await flushMicrotasks();
    const accepted = boardState(view);
    expect(accepted.snapshot.powerOutputs[0]?.value).toBe(1);
    expect(accepted.snapshot.adc[0]?.value).toBe(200);
    expect(accepted.powerUpdatedAt).toBe(11_000);

    vi.setSystemTime(12_000);
    await act(async () => oldAdc.resolve(adc(100)));
    expect(boardState(view)).toEqual(accepted);
    expect(view.current().snapshot).toBe(accepted.snapshot);
    expect(apiMocks.getGpioLayout).toHaveBeenCalledTimes(1);
  });

  it.each(["status", "ADC"])("ignores an old %s error after a newer refresh succeeds", async (phase) => {
    const old = deferred<unknown>();
    if (phase === "status") apiMocks.getStatus.mockReturnValueOnce(old.promise);
    else apiMocks.getAdc.mockReturnValueOnce(old.promise);
    const view = mount();
    await flushMicrotasks();

    vi.setSystemTime(11_000);
    apiMocks.getStatus.mockResolvedValueOnce(status(1));
    await act(async () => { await view.current().refresh(); });
    await flushMicrotasks();
    const accepted = boardState(view);
    const layoutSignal = apiMocks.getGpioLayout.mock.calls[0][0]!;
    await act(async () => old.reject(new Error("old request failed")));

    expect(boardState(view)).toEqual(accepted);
    expect(accepted.connected).toBe(true);
    expect(accepted.error).toBeNull();
    expect(layoutSignal.aborted).toBe(false);
    expect(apiMocks.getGpioLayout).toHaveBeenCalledTimes(1);
  });

  it("does not revive data, clear a newer error, or start GPIO layout from an old status", async () => {
    const oldStatus = deferred<unknown>();
    apiMocks.getStatus.mockReturnValueOnce(oldStatus.promise);
    const view = mount();
    apiMocks.getStatus.mockRejectedValueOnce(new Error("current offline"));
    await act(async () => { await view.current().refresh(); });
    const accepted = boardState(view);
    expect(accepted.connected).toBe(false);
    expect(accepted.hasData).toBe(false);
    expect(accepted.error).toBe("current offline");

    await act(async () => oldStatus.resolve(status(0)));
    expect(boardState(view)).toEqual(accepted);
    expect(apiMocks.getAdc).not.toHaveBeenCalled();
    expect(apiMocks.getGpioLayout).not.toHaveBeenCalled();
  });

  it.each(["success", "failure"])("does not let an old %s finally finish a newer pending refresh", async (outcome) => {
    const oldAdc = deferred<unknown>();
    const newAdc = deferred<unknown>();
    apiMocks.getAdc.mockReturnValueOnce(oldAdc.promise).mockReturnValueOnce(newAdc.promise);
    const view = mount();
    await flushMicrotasks();
    const newer = startRefresh(view);
    await flushMicrotasks();
    const pending = boardState(view);
    expect(pending.loading).toBe(true);
    expect(pending.hasData).toBe(false);

    await act(async () => {
      if (outcome === "success") oldAdc.resolve(adc(100));
      else oldAdc.reject(new Error("old ADC failed"));
    });
    expect(boardState(view)).toEqual(pending);
    expect(apiMocks.getGpioLayout).not.toHaveBeenCalled();

    await act(async () => { newAdc.resolve(adc(200)); await newer; });
    expect(view.current().loading).toBe(false);
    expect(view.current().hasData).toBe(true);
    expect(view.current().snapshot.adc[0]?.value).toBe(200);
  });

  it.each(["success", "failure"])("drops polling %s after switching to a newer live snapshot", async (outcome) => {
    const oldAdc = deferred<unknown>();
    apiMocks.getStatus.mockResolvedValueOnce(status(0));
    apiMocks.getAdc.mockReturnValueOnce(oldAdc.promise);
    const view = mount();
    await flushMicrotasks();
    act(() => view.current().setLive(true));
    await flushMicrotasks();
    const socket = MockWebSocket.instances.at(-1)!;
    vi.setSystemTime(11_000);
    act(() => {
      socket.emitOpen();
      socket.emitMessage({ type: "snapshot", ...status(1) });
    });
    const accepted = boardState(view);
    expect(accepted.live).toBe(true);
    expect(accepted.snapshot.powerOutputs[0]?.value).toBe(1);

    await act(async () => {
      if (outcome === "success") oldAdc.resolve(adc(100));
      else oldAdc.reject(new Error("old polling failed"));
    });
    expect(boardState(view)).toEqual(accepted);
    expect(apiMocks.getGpioLayout).toHaveBeenCalledTimes(1);
  });

  it.each(["status", "ADC"])("ignores a canceled live seed %s error after live reconnects", async (phase) => {
    const view = mount();
    await flushMicrotasks();
    const oldSeed = deferred<unknown>();
    if (phase === "status") apiMocks.getStatus.mockReturnValueOnce(oldSeed.promise);
    else apiMocks.getAdc.mockReturnValueOnce(oldSeed.promise);
    act(() => view.current().setLive(true));
    await flushMicrotasks();
    act(() => view.current().setLive(false));
    await flushMicrotasks();
    act(() => view.current().setLive(true));
    await flushMicrotasks();
    const socket = MockWebSocket.instances.at(-1)!;
    act(() => socket.emitOpen());
    const accepted = boardState(view);
    expect(accepted.live).toBe(true);

    await act(async () => oldSeed.reject(new Error("canceled seed failed")));
    await flushMicrotasks();
    expect(boardState(view)).toEqual(accepted);
    expect(socket.readyState).toBe(MockWebSocket.OPEN);
    expect(apiMocks.createLiveSession).toHaveBeenCalledTimes(1);
  });

  it.each([
    ["manual refresh", "success"], ["manual refresh", "failure"],
    ["live status poll", "success"], ["live status poll", "failure"],
  ])("merges an in-flight %s %s by domain without rolling back newer live state", async (source, outcome) => {
    const { view, socket } = await mountLive();
    const old = deferred<unknown>();
    if (source === "manual refresh") {
      apiMocks.getStatus.mockResolvedValueOnce(httpStatus(0));
      apiMocks.getAdc.mockReturnValueOnce(old.promise);
      void startRefresh(view);
      await flushMicrotasks();
    } else {
      apiMocks.getStatus.mockReturnValueOnce(old.promise);
      await act(async () => vi.advanceTimersByTimeAsync(10_000));
    }
    vi.setSystemTime(21_000);
    act(() => socket.emitMessage(liveStatus(1)));
    vi.setSystemTime(21_500);
    act(() => socket.emitMessage({
      type: "snapshot", watchdog: { armed: true },
      board_monitoring: { runtime: { uptime_ms: 300 } },
    }));
    await flushMicrotasks();
    const accepted = boardState(view);
    expect(accepted.powerUpdatedAt).toBe(21_000);

    vi.setSystemTime(22_000);
    await act(async () => {
      if (outcome === "failure") old.reject(new Error("HTTP failed before the newer live state"));
      else old.resolve(source === "manual refresh" ? adc(100) : httpStatus(0));
    });
    if (outcome === "failure") {
      expect(boardState(view)).toEqual(accepted);
      expect(view.current().snapshot).toBe(accepted.snapshot);
    } else {
      const { snapshot: before, ...stateBefore } = accepted;
      const { snapshot: after, ...stateAfter } = boardState(view);
      expect(stateAfter).toEqual(stateBefore);
      expect(after.mcu).toBe("rp2350a");
      expect(after.usb).toBe("ncm-ecm");
      expect(after.pd?.voltage_mv).toBe(20_000);
      expect(after.pd?.capabilities_valid).toBe(true);
      expect(after.powerOutputs).toEqual([{
        name: "5v_out", state: "on", value: 1,
        signal: "S_C_5V", gp: 7, controllable: false,
      }]);
      for (const field of ["switches", "gpios", "gpioLayoutGroups", "watchdog", "monitoring", "config"] as const) {
        expect(after[field]).toEqual(before[field]);
      }
      expect(after.adc[0]?.value).toBe(source === "manual refresh" ? 100 : undefined);
    }

    // A discarded live HTTP response must not stop subsequent scheduling.
    if (source === "live status poll") {
      const calls = apiMocks.getStatus.mock.calls.length;
      await act(async () => vi.advanceTimersByTimeAsync(10_000));
      expect(apiMocks.getStatus).toHaveBeenCalledTimes(calls + 1);
    }
  });

  it("keeps an unrelated GPIO frame without discarding HTTP power freshness or PD", async () => {
    const { view, socket } = await mountLive();
    const pendingAdc = deferred<unknown>();
    vi.setSystemTime(11_000);
    apiMocks.getStatus.mockResolvedValueOnce(httpStatus(0));
    apiMocks.getAdc.mockReturnValueOnce(pendingAdc.promise);
    const request = startRefresh(view);
    await flushMicrotasks();
    vi.setSystemTime(12_000);
    act(() => socket.emitMessage({
      type: "snapshot", gpios: [{ name: "GP12", pin: 12, direction: "input", value: 1 }],
    }));
    const gpios = view.current().snapshot.gpios;
    expect(view.current().powerUpdatedAt).toBe(10_000);

    vi.setSystemTime(13_000);
    await act(async () => { pendingAdc.resolve(adc(100)); await request; });
    expect(view.current().snapshot.gpios).toEqual(gpios);
    expect(view.current().snapshot.powerOutputs[0]).toMatchObject({ state: "off", signal: "S_C_5V", gp: 7 });
    expect(view.current().snapshot.pd?.voltage_mv).toBe(20_000);
    expect(view.current().powerUpdatedAt).toBe(11_000);
    expect(view.current().lastVerifiedAt).toBe(12_000);
    expect(view.current().connected).toBe(true);
    expect(view.current().error).toBeNull();
  });

  it("lets a live snapshot finish initial loading when a manual refresh supersedes the live seed", async () => {
    const oldPoll = deferred<unknown>();
    const liveSeed = deferred<unknown>();
    const manualAdc = deferred<unknown>();
    apiMocks.getStatus.mockReturnValueOnce(oldPoll.promise).mockReturnValueOnce(liveSeed.promise);
    const view = mount();
    act(() => view.current().setLive(true));
    await flushMicrotasks();
    apiMocks.getStatus.mockResolvedValueOnce(status(0));
    apiMocks.getAdc.mockReturnValueOnce(manualAdc.promise);
    const manual = startRefresh(view);
    await flushMicrotasks();

    // The superseded seed can still establish this session's socket, but its
    // HTTP result must not commit or start a GPIO layout read.
    await act(async () => liveSeed.resolve(status(0)));
    await flushMicrotasks();
    expect(view.current().loading).toBe(true);
    expect(view.current().hasData).toBe(false);
    expect(apiMocks.getAdc).toHaveBeenCalledTimes(1);
    expect(apiMocks.getGpioLayout).not.toHaveBeenCalled();
    const socket = MockWebSocket.instances.at(-1)!;
    act(() => {
      socket.emitOpen();
      socket.emitMessage(liveStatus(1));
    });
    await flushMicrotasks();
    const accepted = boardState(view);
    expect(accepted.loading).toBe(false);
    expect(accepted.hasData).toBe(true);
    expect(accepted.connected).toBe(true);
    expect(accepted.error).toBeNull();
    expect(accepted.snapshot.powerOutputs[0]?.value).toBe(1);

    expect(apiMocks.getGpioLayout).toHaveBeenCalledTimes(1);
    expect(accepted.gpioPhysicalLayout).not.toBeNull();
    await act(async () => { oldPoll.resolve(status(0)); manualAdc.resolve(adc(100)); await manual; });
    const completed = boardState(view);
    expect({ ...completed, snapshot: accepted.snapshot }).toEqual(accepted);
    expect(completed.snapshot.powerOutputs[0]?.value).toBe(1);
    expect(completed.snapshot.gpios).toEqual(accepted.snapshot.gpios);
    expect(apiMocks.getGpioLayout).toHaveBeenCalledTimes(1);
  });

  it.each(["success", "failure"])("invalidates a pending polling %s while hidden, then resumes a fresh session", async (outcome) => {
    const oldStatus = deferred<unknown>();
    apiMocks.getStatus.mockReturnValueOnce(oldStatus.promise);
    const view = mount();
    setHidden(true);
    const hidden = boardState(view);
    expect(hidden.loading).toBe(false);
    await act(async () => {
      if (outcome === "success") oldStatus.resolve(status(0));
      else oldStatus.reject(new Error("hidden poll failed"));
    });
    expect(boardState(view)).toEqual(hidden);
    expect(apiMocks.getAdc).not.toHaveBeenCalled();
    expect(apiMocks.getGpioLayout).not.toHaveBeenCalled();

    apiMocks.getStatus.mockResolvedValueOnce(status(1));
    setHidden(false);
    await flushMicrotasks();
    expect(view.current().snapshot.powerOutputs[0]?.value).toBe(1);
    expect(view.current().hasData).toBe(true);
    expect(view.current().loading).toBe(false);
  });
});
