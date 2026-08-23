import { act } from "react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import {
  apiMocks,
  flushMicrotasks,
  renderBoard,
  renderLiveBoard,
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
    expect(preserved.watchdog.timeout_ms).toBe(16000);
    expect(preserved.monitoring.heap?.free_bytes).toBe(1024);
    expect(preserved.powerCaptureProtocol).toBe("host-stream-v1");
    expect(preserved.config?.savedCount).toBe(2);

    // When a later snapshot carries only GPIOs
    act(() => {
      socket.emitMessage({ type: "snapshot", gpios: [{ name: "GP8", pin: 8 }] });
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
    expect(updated.powerOutputs[0]?.state).toBe("on");
    view.close();
  });
});
