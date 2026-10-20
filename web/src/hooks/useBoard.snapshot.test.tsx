import { act } from "react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { EMPTY_PD_STATUS } from "@/lib/pd";
import type { PdStatus } from "@/lib/types";
import {
  apiMocks,
  flushMicrotasks,
  renderBoard,
  renderLiveBoard,
  setHidden,
  setupUseBoardHarness,
  teardownUseBoardHarness,
} from "./useBoard.testUtils";

function richStatus() {
  return {
    mcu: "rp2350a",
    usb: "ncm-ecm",
    power_capture_protocol: "host-stream-v1",
    power_outputs: [
      { name: "5v_out", signal: "S_C_5V", gp: 7, controllable: true, state: "on", value: 1 },
    ],
    gpios: [
      {
        name: "GP7", pin: 7, note: "n", value: 1, direction: "output",
        layoutGroup: "g", layoutLabel: "l", layoutRow: 2, layoutColumn: 3,
      },
    ],
    gpio_layout_groups: [{ name: "G", edge: "right", order: 0 }],
    switches: { tf: { route: "sd", routes: ["sd", "emmc"], requires_confirm: false } },
    watchdog: {
      supported: true, automatic: true, healthy: true, armed: true,
      timeout_ms: 16000, bootloader_on_timeout: true, failing_service: "svc",
    },
    board_monitoring: {
      temperature: { available: true },
      heap: { available: true, free_bytes: 1024 },
      memory: { available: true, physical: { total_bytes: 264 } },
      runtime: { available: true, uptime_ms: 60 },
      cpu: { available: false },
    },
    config: { available: true, reason: "", saved_count: 2, pending_count: 0 },
  };
}

describe("useBoard board snapshot boundary", () => {
  beforeEach(setupUseBoardHarness);
  afterEach(teardownUseBoardHarness);

  it("maps the HTTP status and ADC poll into the snapshot", async () => {
    // Given a rich firmware status response
    apiMocks.getStatus.mockResolvedValue(richStatus());
    apiMocks.getAdc.mockResolvedValue({
      readings: [{
        name: "5v_out", signal: "S_C_5V", sensor_channel: "current", unit: "A",
        current_ua: 2500, power_enabled: true,
      }],
    });

    // When the hook polls
    const view = renderBoard();
    await flushMicrotasks();

    // Then every documented field is mapped
    const { snapshot } = view.current();
    expect(snapshot.mcu).toBe("rp2350a");
    expect(snapshot.usb).toBe("ncm-ecm");
    expect(snapshot.powerCaptureProtocol).toBe("host-stream-v1");
    expect(snapshot.powerOutputs).toEqual([
      { name: "5v_out", signal: "S_C_5V", gp: 7, controllable: true, state: "on", value: 1 },
    ]);
    expect(snapshot.gpios).toEqual([
      {
        name: "GP7", pin: 7, note: "n", value: 1, direction: "output",
        layoutGroup: "g", layoutLabel: "l", layoutRow: 2, layoutColumn: 3,
      },
    ]);
    expect(snapshot.gpioLayoutGroups).toEqual([{ name: "G", edge: "right", order: 0 }]);
    expect(snapshot.switches.tf).toEqual({
      route: "sd", routes: ["sd", "emmc"], requires_confirm: false,
    });
    expect(snapshot.watchdog).toEqual({
      supported: true, automatic: true, healthy: true, armed: true,
      timeout_ms: 16000, bootloader_on_timeout: true, failing_service: "svc",
    });
    expect(snapshot.monitoring.heap).toEqual({ available: true, free_bytes: 1024 });
    expect(snapshot.monitoring.memory?.physical?.total_bytes).toBe(264);
    expect(snapshot.adc[0]?.value).toBe(2500);
    expect(snapshot.config).toEqual({
      available: true, reason: "", savedCount: 2, pendingCount: 0,
    });
    expect(view.current().hasData).toBe(true);
    expect(view.current().connected).toBe(true);
    expect(view.current().loading).toBe(false);
    expect(view.current().error).toBeNull();
    view.close();
  });

  it("keeps documented fallbacks for missing and malformed fields", async () => {
    // Given a sparse and malformed status response
    apiMocks.getStatus.mockResolvedValue({
      power_outputs: ["junk", { name: 5 }],
      gpios: "nope",
    });
    apiMocks.getAdc.mockResolvedValue(null);

    // When the hook polls
    const view = renderBoard();
    await flushMicrotasks();

    // Then defaults and per-field fallbacks apply
    const { snapshot } = view.current();
    expect(snapshot.powerOutputs).toEqual([
      { name: "", signal: undefined, gp: undefined, controllable: false, state: "", value: null },
    ]);
    expect(snapshot.gpios).toEqual([]);
    expect(snapshot.gpioLayoutGroups).toEqual([]);
    expect(snapshot.switches).toEqual({});
    expect(snapshot.watchdog.supported).toBe(false);
    expect(snapshot.monitoring.temperature).toEqual({ available: false });
    expect(snapshot.adc).toEqual([]);
    expect(snapshot.config).toBeUndefined();
    view.close();
  });

  it("preserves HTTP-only power metadata when a WebSocket snapshot arrives", async () => {
    // Given an HTTP-seeded live session
    apiMocks.getStatus.mockResolvedValue(richStatus());
    const { view, socket } = await renderLiveBoard();

    // When a WebSocket snapshot omits the HTTP-only metadata
    act(() => {
      socket.emitMessage({
        type: "snapshot",
        power_outputs: [{ name: "5v_out", state: "off", value: 0 }],
      });
    });

    // Then the metadata survives while the state updates
    expect(view.current().snapshot.powerOutputs).toEqual([
      { name: "5v_out", signal: "S_C_5V", gp: 7, controllable: true, state: "off", value: 0 },
    ]);
    view.close();
  });

  it("preserves previous values for fields a WebSocket snapshot omits", async () => {
    // Given an HTTP-seeded live session
    apiMocks.getStatus.mockResolvedValue(richStatus());
    const { view, socket } = await renderLiveBoard();

    // When an empty WebSocket snapshot arrives
    act(() => {
      socket.emitMessage({ type: "snapshot" });
    });

    // Then all previous values are preserved
    const preserved = view.current().snapshot;
    expect(preserved.powerOutputs[0]?.signal).toBe("S_C_5V");
    expect(preserved.gpios[0]?.layoutRow).toBe(2);
    expect(preserved.gpioLayoutGroups).toEqual([{ name: "G", edge: "right", order: 0 }]);
    expect(preserved.watchdog.timeout_ms).toBe(16000);
    expect(preserved.monitoring.heap?.free_bytes).toBe(1024);
    expect(preserved.powerCaptureProtocol).toBe("host-stream-v1");
    expect(preserved.config?.savedCount).toBe(2);

    // When a later snapshot carries only GPIOs
    act(() => {
      socket.emitMessage({
        type: "snapshot", gpios: [{ name: "GP8", pin: 8 }],
        gpio_layout_groups: [{ name: "NEXT", edge: "unknown", order: 1 }],
      });
    });

    // Then GPIOs are replaced with per-field fallbacks and switches are kept
    const updated = view.current().snapshot;
    expect(updated.gpios).toEqual([
      {
        name: "GP8", pin: 8, note: "", value: 0, direction: "input",
        layoutGroup: undefined, layoutLabel: undefined,
        layoutRow: undefined, layoutColumn: undefined,
      },
    ]);
    expect(updated.switches.tf?.route).toBe("sd");
    expect(updated.gpioLayoutGroups).toEqual([{ name: "NEXT", edge: "unknown", order: 1 }]);
    expect(updated.powerOutputs[0]?.state).toBe("on");
    view.close();
  });
});

function pdStatus(overrides: Partial<PdStatus> = {}): PdStatus {
  return {
    ...EMPTY_PD_STATUS,
    available: true,
    pd: true,
    protocol: "pd",
    ...overrides,
  };
}

function discoveredPd(): PdStatus {
  return pdStatus({
    capabilities_valid: true,
    fixed_profiles: [{ voltage_mv: 9000, max_current_ma: 3000 }],
    pps_ranges: [{
      min_voltage_mv: 3300, max_voltage_mv: 11000, step_mv: 100, max_current_ma: 3000,
    }],
    avs_ranges: [{
      min_voltage_mv: 15000, max_voltage_mv: 20000, step_mv: 100, max_power_w: 65,
    }],
  });
}

async function advancePolling(ms: number): Promise<void> {
  await act(async () => {
    await vi.advanceTimersByTimeAsync(ms);
  });
  await flushMicrotasks();
}

describe("useBoard on-demand PD capability discovery", () => {
  beforeEach(() => {
    setupUseBoardHarness();
    vi.useFakeTimers();
    vi.setSystemTime(10000);
  });
  afterEach(() => {
    vi.useRealTimers();
    teardownUseBoardHarness();
  });

  it("uses only status and ADC on regular inactive or already-cached polls", async () => {
    const view = renderBoard();
    await flushMicrotasks();
    await advancePolling(4000);
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(3);
    expect(apiMocks.getAdc).toHaveBeenCalledTimes(3);
    expect(apiMocks.getPd).not.toHaveBeenCalled();

    apiMocks.getStatus.mockResolvedValue({ pd: discoveredPd() });
    await advancePolling(4000);
    expect(view.current().snapshot.pd?.capabilities_valid).toBe(true);
    expect(apiMocks.getPd).not.toHaveBeenCalled();
    view.close();
  });

  it("coalesces active invalid polls and preserves a discovery across a capability-omitting partial update", async () => {
    apiMocks.getStatus.mockResolvedValue({ pd: pdStatus() });
    let resolvePd!: (value: PdStatus) => void;
    apiMocks.getPd.mockImplementation(() => new Promise((resolve) => { resolvePd = resolve; }));
    const view = renderBoard();
    await flushMicrotasks();
    expect(apiMocks.getPd).toHaveBeenCalledTimes(1);
    expect(view.current().connected).toBe(true);
    expect(view.current().loading).toBe(false);
    expect(apiMocks.getAdc.mock.invocationCallOrder[0]).toBeLessThan(
      apiMocks.getPd.mock.invocationCallOrder[0]!,
    );
    await advancePolling(4000);
    expect(apiMocks.getPd).toHaveBeenCalledTimes(1);

    await act(async () => resolvePd(discoveredPd()));
    await flushMicrotasks();
    apiMocks.getStatus.mockResolvedValue({ pd: { voltage_mv: 9000 } });
    await advancePolling(6000);
    expect(apiMocks.getPd).toHaveBeenCalledTimes(1);
    expect(view.current().snapshot.pd).toEqual({ ...discoveredPd(), voltage_mv: 9000 });
    view.close();
  });

  it("clears an explicitly invalid short-status table without rediscovering on every two-second poll", async () => {
    apiMocks.getStatus.mockResolvedValue({ pd: pdStatus() });
    apiMocks.getPd.mockResolvedValue(discoveredPd());
    const view = renderBoard();
    await flushMicrotasks();
    expect(apiMocks.getPd).toHaveBeenCalledTimes(1);
    expect(view.current().snapshot.pd?.capabilities_valid).toBe(true);
    await advancePolling(2000);
    expect(view.current().snapshot.pd?.capabilities_valid).toBe(false);
    expect(view.current().snapshot.pd?.fixed_profiles).toEqual([]);
    expect(view.current().snapshot.pd?.pps_ranges).toEqual([]);
    expect(view.current().snapshot.pd?.avs_ranges).toEqual([]);
    expect(apiMocks.getPd).toHaveBeenCalledTimes(1);
    await advancePolling(2999);
    expect(apiMocks.getPd).toHaveBeenCalledTimes(1);
    await advancePolling(1);
    expect(apiMocks.getPd).toHaveBeenCalledTimes(2);
    expect(view.current().snapshot.pd).toEqual(discoveredPd());
    view.close();
  });

  it.each(["request failure", "invalid capabilities"])(
    "backs off at least five seconds after %s without disconnecting the board",
    async (failure) => {
      apiMocks.getStatus.mockResolvedValue({ pd: pdStatus() });
      if (failure === "request failure") {
        apiMocks.getPd.mockRejectedValueOnce(new Error("PD capability read timed out"));
      } else {
        apiMocks.getPd.mockResolvedValueOnce(pdStatus());
      }
      apiMocks.getPd.mockResolvedValue(discoveredPd());
      const view = renderBoard();
      await flushMicrotasks();
      expect(apiMocks.getPd).toHaveBeenCalledTimes(1);
      expect(view.current().connected).toBe(true);
      expect(view.current().hasData).toBe(true);
      expect(view.current().error).toBeNull();

      await advancePolling(4999);
      await act(async () => { await view.current().refresh(); });
      expect(apiMocks.getPd).toHaveBeenCalledTimes(1);
      await advancePolling(1);
      await flushMicrotasks();
      expect(apiMocks.getPd).toHaveBeenCalledTimes(2);
      expect(view.current().snapshot.pd?.capabilities_valid).toBe(true);
      expect(view.current().connected).toBe(true);
      expect(view.current().error).toBeNull();
      view.close();
    },
  );

  it.each(["success", "failure"])(
    "resets cached capabilities and retry backoff on inactive/replug after %s",
    async (outcome) => {
      apiMocks.getStatus.mockResolvedValue({ pd: pdStatus() });
      if (outcome === "failure") {
        apiMocks.getPd.mockRejectedValueOnce(new Error("capabilities unavailable"));
      }
      apiMocks.getPd.mockResolvedValue(discoveredPd());
      const view = renderBoard();
      await flushMicrotasks();
      expect(apiMocks.getPd).toHaveBeenCalledTimes(1);

      apiMocks.getStatus.mockResolvedValue({ pd: pdStatus({ pd: false }) });
      await act(async () => { await view.current().refresh(); });
      expect(view.current().snapshot.pd?.capabilities_valid).toBe(false);
      expect(view.current().snapshot.pd?.fixed_profiles).toEqual([]);
      expect(view.current().snapshot.pd?.pps_ranges).toEqual([]);
      expect(view.current().snapshot.pd?.avs_ranges).toEqual([]);
      expect(apiMocks.getPd).toHaveBeenCalledTimes(1);

      apiMocks.getStatus.mockResolvedValue({ pd: pdStatus() });
      await act(async () => { await view.current().refresh(); });
      await flushMicrotasks();
      expect(apiMocks.getPd).toHaveBeenCalledTimes(2);
      expect(view.current().snapshot.pd?.capabilities_valid).toBe(true);
      view.close();
    },
  );

  it("invalidates capabilities at the live HTTP seed and rediscovers on the five-second cooldown", async () => {
    apiMocks.getStatus.mockResolvedValue({ pd: pdStatus() });
    apiMocks.getPd.mockResolvedValue(discoveredPd());
    const { view, socket } = await renderLiveBoard();
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(2);
    expect(apiMocks.getPd).toHaveBeenCalledTimes(1);
    expect(view.current().snapshot.pd?.capabilities_valid).toBe(false);
    expect(view.current().snapshot.pd?.fixed_profiles).toEqual([]);

    act(() => socket.emitMessage({ type: "snapshot" }));
    expect(view.current().snapshot.pd?.capabilities_valid).toBe(false);
    expect(apiMocks.getPd).toHaveBeenCalledTimes(1);
    await advancePolling(4999);
    expect(apiMocks.getPd).toHaveBeenCalledTimes(1);
    await advancePolling(1);
    expect(apiMocks.getPd).toHaveBeenCalledTimes(2);
    expect(view.current().snapshot.pd).toEqual(discoveredPd());
    act(() => socket.emitMessage({ type: "snapshot", pd: { voltage_mv: 9000 } }));
    expect(view.current().snapshot.pd).toEqual({ ...discoveredPd(), voltage_mv: 9000 });
    view.close();
  });

  it("refreshes live short status only every ten seconds, keeps ADC history and cached capabilities, and cleans up", async () => {
    apiMocks.getStatus.mockResolvedValue({ pd: discoveredPd() });
    const { view } = await renderLiveBoard();
    const adc = view.current().snapshot.adc;
    apiMocks.getStatus.mockResolvedValue({ pd: { ...discoveredPd(), voltage_mv: 9000 } });
    await advancePolling(9999);
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(2);
    await advancePolling(1);
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(3);
    expect(apiMocks.getAdc).toHaveBeenCalledTimes(2);
    expect(apiMocks.getPd).not.toHaveBeenCalled();
    expect(view.current().snapshot.adc).toBe(adc);
    expect(view.current().snapshot.pd).toEqual({ ...discoveredPd(), voltage_mv: 9000 });

    apiMocks.getStatus.mockResolvedValue({ pd: pdStatus({ pd: false }) });
    await advancePolling(10_000);
    expect(view.current().snapshot.pd?.capabilities_valid).toBe(false);
    expect(view.current().snapshot.pd?.fixed_profiles).toEqual([]);
    expect(apiMocks.getPd).not.toHaveBeenCalled();
    view.close();
    await advancePolling(20_000);
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(4);
  });

  it("serializes slow live short reads instead of accumulating interval requests", async () => {
    apiMocks.getStatus.mockResolvedValue({ pd: discoveredPd() });
    const { view } = await renderLiveBoard();
    let finish!: (value: unknown) => void;
    apiMocks.getStatus.mockImplementationOnce(() => new Promise((resolve) => { finish = resolve; }));
    await advancePolling(30_000);
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(3);
    await act(async () => finish({ pd: discoveredPd() }));
    await advancePolling(9999);
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(3);
    await advancePolling(1);
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(4);
    view.close();
  });

  it.each(["request failure", "invalid capabilities"])("retries %s autonomously in live mode after five seconds", async (failure) => {
    apiMocks.getStatus.mockResolvedValue({ pd: pdStatus() });
    if (failure === "request failure") apiMocks.getPd.mockRejectedValueOnce(new Error("temporary failure"));
    else apiMocks.getPd.mockResolvedValueOnce(pdStatus());
    apiMocks.getPd.mockResolvedValue(discoveredPd());
    const { view } = await renderLiveBoard();
    await advancePolling(4999);
    expect(apiMocks.getPd).toHaveBeenCalledTimes(1);
    await advancePolling(1);
    expect(apiMocks.getPd).toHaveBeenCalledTimes(2);
    expect(view.current().snapshot.pd?.capabilities_valid).toBe(true);
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(2);
    expect(apiMocks.getAdc).toHaveBeenCalledTimes(2);
    expect(view.current().connected).toBe(true);
    expect(view.current().error).toBeNull();
    view.close();
  });

  it.each(["hidden", "disconnected", "inactive", "unmounted"])("cancels capability retries when %s", async (condition) => {
    apiMocks.getStatus.mockResolvedValue({ pd: pdStatus() });
    apiMocks.getPd.mockRejectedValue(new Error("temporary failure"));
    const { view, socket } = await renderLiveBoard();
    if (condition === "hidden") setHidden(true);
    if (condition === "disconnected") {
      apiMocks.getStatus.mockRejectedValue(new Error("offline"));
      await act(async () => view.current().refresh());
    }
    if (condition === "inactive") {
      act(() => socket.emitMessage({ type: "snapshot", pd: pdStatus({ pd: false }) }));
    }
    if (condition === "unmounted") view.close();
    await advancePolling(5000);
    expect(apiMocks.getPd).toHaveBeenCalledTimes(1);
    if (condition !== "unmounted") view.close();
  });

  it("pauses optional PD traffic even when a hidden armed capture retains its live socket, then resumes discovery", async () => {
    apiMocks.getStatus.mockResolvedValue({ power_capture_protocol: "host-stream-v1", pd: pdStatus() });
    apiMocks.getPd.mockRejectedValueOnce(new Error("temporary failure"));
    apiMocks.getPd.mockResolvedValue(discoveredPd());
    const { view, socket } = await renderLiveBoard();
    let arm!: Promise<void>;
    act(() => {
      arm = view.current().armCapture({
        trigger: "manual", source: "5v_out", edge: "rising", thresholdUa: 0,
        rateHz: 250, preSamples: 0, postSamples: 0, streaming: true,
      });
    });
    await flushMicrotasks();
    act(() => socket.emitMessage({ type: "result", command: "capture_arm", id: "web-capture" }));
    await arm;
    setHidden(true);
    await advancePolling(20_000);
    expect(socket.readyState).toBe(1);
    expect(apiMocks.getStatus).toHaveBeenCalledTimes(2);
    expect(apiMocks.getPd).toHaveBeenCalledTimes(1);
    setHidden(false);
    await flushMicrotasks();
    expect(apiMocks.getPd).toHaveBeenCalledTimes(2);
    expect(view.current().snapshot.pd?.capabilities_valid).toBe(true);
    view.close();
  });

  it("ignores a pending old-source capability response after PD disconnects", async () => {
    apiMocks.getStatus.mockResolvedValue({ pd: pdStatus() });
    let resolvePd!: (value: PdStatus) => void;
    apiMocks.getPd.mockImplementation(() => new Promise((resolve) => { resolvePd = resolve; }));
    const { view, socket } = await renderLiveBoard();
    act(() => socket.emitMessage({ type: "snapshot", pd: pdStatus({ pd: false }) }));
    await act(async () => resolvePd(discoveredPd()));
    expect(view.current().snapshot.pd?.capabilities_valid).toBe(false);
    expect(view.current().snapshot.pd?.fixed_profiles).toEqual([]);
    await advancePolling(6000);
    expect(apiMocks.getPd).toHaveBeenCalledTimes(1);
    view.close();
  });
});
