import { act } from "react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { EMPTY_PD_STATUS } from "@/lib/pd";
import {
  apiMocks,
  flushMicrotasks,
  renderBoard,
  renderLiveBoard,
  setupUseBoardHarness,
  teardownUseBoardHarness,
} from "./useBoard.testUtils";

const POWER_STATUS = {
  power_capture_protocol: "host-stream-v1",
  power_outputs: [{ name: "5v_out", state: "on" }],
  gpios: [],
  switches: {},
  watchdog: {},
  board_monitoring: {},
};

function currentAdcResponse(currentUa: number) {
  return {
    readings: [{
      name: "5v_out",
      signal: "S_C_5V",
      sensor_channel: "current",
      unit: "A",
      power_enabled: true,
      current_ua: currentUa,
    }],
  };
}

describe("useBoard REST mutations", () => {
  beforeEach(setupUseBoardHarness);
  afterEach(teardownUseBoardHarness);

  it("resolves setPower when the device confirms the requested state", async () => {
    // Given a polling board whose power endpoint confirms the mutation
    apiMocks.setPower.mockResolvedValue({ power_output: { name: "5v_out", state: "on" } });
    const view = renderBoard();
    await flushMicrotasks();

    // When the rail is switched on
    await act(async () => {
      await view.current().setPower("5v_out", true);
    });

    // Then the exact API call is forwarded
    expect(apiMocks.setPower).toHaveBeenCalledWith("5v_out", true);
    view.close();
  });

  it("rejects setPower when the device reports a different state", async () => {
    // Given a power endpoint that confirms the opposite state
    apiMocks.setPower.mockResolvedValue({ power_output: { name: "5v_out", state: "off" } });
    const view = renderBoard();
    await flushMicrotasks();

    // When the rail is switched on
    // Then the confirmation mismatch rejects with the exact error
    await expect(view.current().setPower("5v_out", true)).rejects.toThrow(
      "Power output 5v_out did not confirm state on",
    );
    view.close();
  });

  it("refreshes the snapshot after setPower in polling mode", async () => {
    // Given a polling board
    apiMocks.setPower.mockResolvedValue({ power_output: { name: "5v_out", state: "on" } });
    const view = renderBoard();
    await flushMicrotasks();
    const statusCallsBefore = apiMocks.getStatus.mock.calls.length;

    // When a mutation completes
    await act(async () => {
      await view.current().setPower("5v_out", true);
    });

    // Then one more HTTP refresh ran
    expect(apiMocks.getStatus.mock.calls.length).toBe(statusCallsBefore + 1);
    view.close();
  });

  it("does not refresh after setPower in live mode", async () => {
    // Given a live board session
    apiMocks.setPower.mockResolvedValue({ power_output: { name: "5v_out", state: "on" } });
    const { view } = await renderLiveBoard();
    const statusCallsBefore = apiMocks.getStatus.mock.calls.length;

    // When a mutation completes
    await act(async () => {
      await view.current().setPower("5v_out", true);
    });
    await flushMicrotasks();

    // Then no HTTP refresh ran; the live stream owns updates
    expect(apiMocks.getStatus.mock.calls.length).toBe(statusCallsBefore);
    view.close();
  });

  it("parses the PD PUT response into the live snapshot without status, ADC or capability reads", async () => {
    const initial = {
      ...EMPTY_PD_STATUS, available: true, pd: true, protocol: "pd", capabilities_valid: true,
      fixed_profiles: [{ voltage_mv: 5000, max_current_ma: 3000 }, { voltage_mv: 9000, max_current_ma: 2000 }],
    };
    apiMocks.getStatus.mockResolvedValue({ ...POWER_STATUS, pd: initial });
    apiMocks.setPd.mockResolvedValue({
      available: true, pd: true, mode: 1, voltage_mv: 9000, pps_mv: 9000,
      max_current_ma: 2000, protocol: "pd",
    });
    const { view } = await renderLiveBoard();
    const before = [apiMocks.getStatus.mock.calls.length, apiMocks.getAdc.mock.calls.length];
    await act(async () => view.current().setPd(1, 9000));
    expect(apiMocks.setPd).toHaveBeenCalledWith(1, 9000);
    expect(view.current().snapshot.pd).toEqual({
      ...initial, mode: 1, voltage_mv: 9000, pps_mv: 9000, max_current_ma: 2000,
    });
    expect([apiMocks.getStatus.mock.calls.length, apiMocks.getAdc.mock.calls.length]).toEqual(before);
    expect(apiMocks.getPd).not.toHaveBeenCalled();
    view.close();
  });

  it("clears cached PD capabilities when the PUT response explicitly invalidates them", async () => {
    apiMocks.getStatus.mockResolvedValue({ ...POWER_STATUS, pd: {
      ...EMPTY_PD_STATUS, available: true, pd: true, capabilities_valid: true,
      fixed_profiles: [{ voltage_mv: 9000, max_current_ma: 3000 }],
    } });
    apiMocks.setPd.mockResolvedValue({ capabilities_valid: false, voltage_mv: 9000 });
    apiMocks.getPd.mockRejectedValue(new Error("capability read temporarily unavailable"));
    const { view } = await renderLiveBoard();
    await act(async () => view.current().setPd(0, 9000));
    await flushMicrotasks();
    expect(view.current().snapshot.pd?.capabilities_valid).toBe(false);
    expect(view.current().snapshot.pd?.fixed_profiles).toEqual([]);
    expect(apiMocks.getPd).toHaveBeenCalledTimes(1);
    view.close();
  });

  it("reads status before ADC and returns the reported state and current", async () => {
    // Given a device reporting the rail on with a positive current reading
    apiMocks.getStatus.mockResolvedValue(POWER_STATUS);
    apiMocks.getAdc.mockResolvedValue(currentAdcResponse(1234));
    const view = renderBoard();
    await flushMicrotasks();

    // When the rail state is read back
    let result: { state: string; currentUa: number } | null = null;
    await act(async () => {
      result = await view.current().readPower("5v_out");
    });

    // Then the reads ran sequentially in status-then-ADC order
    const statusOrder = apiMocks.getStatus.mock.invocationCallOrder.at(-1);
    const adcOrder = apiMocks.getAdc.mock.invocationCallOrder.at(-1);
    if (statusOrder === undefined || adcOrder === undefined) {
      throw new Error("readPower did not perform both reads");
    }
    expect(statusOrder).toBeLessThan(adcOrder);
    expect(result).toEqual({ state: "on", currentUa: 1234, voltageUv: 0 });
    view.close();
  });

  it("rejects readPower when the device does not report the output", async () => {
    // Given a device that does not list the requested rail
    apiMocks.getStatus.mockResolvedValue(POWER_STATUS);
    const view = renderBoard();
    await flushMicrotasks();

    // When an unknown rail is read
    // Then the exact missing-output error is raised
    await expect(view.current().readPower("9v_out")).rejects.toThrow(
      "Power output 9v_out was not reported by the device",
    );
    view.close();
  });

  it("clamps a negative current reading to zero", async () => {
    // Given a negative current reading from the device
    apiMocks.getStatus.mockResolvedValue(POWER_STATUS);
    apiMocks.getAdc.mockResolvedValue(currentAdcResponse(-500));
    const view = renderBoard();
    await flushMicrotasks();

    // When the rail state is read back
    let result: { state: string; currentUa: number } | null = null;
    await act(async () => {
      result = await view.current().readPower("5v_out");
    });

    // Then the current is clamped to zero
    expect(result).toEqual({ state: "on", currentUa: 0, voltageUv: 0 });
    view.close();
  });

  it("forwards setSwitch and setGpio unchanged and refreshes in polling mode", async () => {
    // Given a polling board
    const view = renderBoard();
    await flushMicrotasks();
    const statusCallsBefore = apiMocks.getStatus.mock.calls.length;

    // When a switch route and a GPIO output are set
    await act(async () => {
      await view.current().setSwitch("vin", "1.8v");
    });
    await act(async () => {
      await view.current().setGpio("GP7", "output", 1);
    });

    // Then both calls are forwarded unchanged and each triggers a refresh
    expect(apiMocks.setSwitch).toHaveBeenCalledWith("vin", "1.8v");
    expect(apiMocks.setGpio).toHaveBeenCalledWith("GP7", "output", 1);
    expect(apiMocks.getStatus.mock.calls.length).toBe(statusCallsBefore + 2);
    view.close();
  });

  it("enters the bootloader without triggering a refresh", async () => {
    // Given a polling board
    const view = renderBoard();
    await flushMicrotasks();
    const statusCallsBefore = apiMocks.getStatus.mock.calls.length;

    // When bootloader entry is requested
    await act(async () => {
      await view.current().enterBootloader();
    });

    // Then the call is forwarded and no refresh follows the reboot
    expect(apiMocks.enterBootloader).toHaveBeenCalledOnce();
    expect(apiMocks.getStatus.mock.calls.length).toBe(statusCallsBefore);
    view.close();
  });
});
