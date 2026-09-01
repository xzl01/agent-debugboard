import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import { LanguageProvider } from "@/lib/i18n";
import { ThemeProvider } from "@/lib/theme";
import type { BoardSnapshot } from "@/lib/types";
import { StatusBar } from "./StatusBar";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const emptySnapshot: BoardSnapshot = {
  powerOutputs: [],
  switches: {},
  gpios: [],
  watchdog: {
    supported: false,
    automatic: false,
    healthy: false,
    armed: false,
    timeout_ms: 0,
    bootloader_on_timeout: false,
    failing_service: "",
  },
  monitoring: {
    temperature: { available: false },
    heap: { available: false },
    runtime: { available: false },
    cpu: { available: false },
  },
  adc: [],
};

function render(
  snapshot: BoardSnapshot,
  runtime: { readonly logicAnalyzerActive: boolean; readonly uartBridgeActive: boolean } = {
    logicAnalyzerActive: false,
    uartBridgeActive: false,
  },
) {
  localStorage.setItem("theme", "light");
  localStorage.setItem("lang", "en");
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  const renderStatus = (
    nextSnapshot: BoardSnapshot,
    connected = true,
    nextRuntime = runtime,
  ) => {
    root.render(
      <ThemeProvider>
        <LanguageProvider>
          <StatusBar
            snapshot={nextSnapshot}
            connected={connected}
            loading={false}
            auto
            setAuto={vi.fn()}
            live={false}
            setLive={vi.fn()}
            onRefresh={vi.fn()}
            logicAnalyzerActive={nextRuntime.logicAnalyzerActive}
            uartBridgeActive={nextRuntime.uartBridgeActive}
          />
        </LanguageProvider>
      </ThemeProvider>,
    );
  };
  act(() => renderStatus(snapshot));
  return {
    host,
    update: (
      nextSnapshot: BoardSnapshot,
      connected = true,
      nextRuntime = runtime,
    ) => act(() => renderStatus(nextSnapshot, connected, nextRuntime)),
    close: () => act(() => {
      root.unmount();
      host.remove();
    }),
  };
}

const indicatorStates = (host: HTMLElement) =>
  [...host.querySelectorAll("[data-linkr-arm]")].map((arm) => ({
    indicator: arm.getAttribute("data-indicator"),
    active: arm.getAttribute("data-active"),
  }));

describe("StatusBar Linkr indicator", () => {
  afterEach(() => {
    vi.restoreAllMocks();
    document.body.replaceChildren();
    localStorage.clear();
  });

  it("maps the three main rails to the upper-left, upper-right, and lower-left arms", () => {
    const power: BoardSnapshot = {
      ...emptySnapshot,
      powerOutputs: [
        { name: "20v_out", controllable: true, state: "on", value: 1 },
        { name: "vdd_5v", controllable: true, state: "on", value: 1 },
        { name: "5v_out", controllable: true, state: "off", value: 0 },
        { name: "12v_out", controllable: true, state: "on", value: 1 },
      ],
    };
    const view = render(power, { logicAnalyzerActive: false, uartBridgeActive: true });
    expect(indicatorStates(view.host)).toEqual([
      { indicator: "5v_out", active: "false" },
      { indicator: "12v_out", active: "true" },
      { indicator: "uart_bridge", active: "true" },
      { indicator: "20v_out", active: "true" },
    ]);
    view.close();
  });

  it("exposes the neutral ready backplate, center heartbeat, and logic-analyzer rotor", () => {
    const view = render(emptySnapshot, {
      logicAnalyzerActive: true,
      uartBridgeActive: false,
    });
    const logo = view.host.querySelector('[data-testid="linkr-logo"]');
    expect(logo?.getAttribute("data-state")).toBe("ready");
    expect(logo?.getAttribute("data-logic-analyzer-active")).toBe("true");
    expect(view.host.querySelector("[data-linkr-backplate]")).not.toBeNull();
    expect(view.host.querySelector("[data-linkr-heartbeat]")).not.toBeNull();
    expect(view.host.querySelector("[data-linkr-rotor]")).not.toBeNull();

    view.update(emptySnapshot, false, {
      logicAnalyzerActive: true,
      uartBridgeActive: true,
    });
    expect(logo?.getAttribute("data-state")).toBe("offline");
    view.close();
  });

  it("updates rail state directly instead of replaying historical deltas", () => {
    const view = render({
      ...emptySnapshot,
      powerOutputs: [{ name: "5v_out", controllable: true, state: "off", value: 0 }],
    });
    expect(indicatorStates(view.host)[0]?.active).toBe("false");
    view.update({
      ...emptySnapshot,
      powerOutputs: [{ name: "5v_out", controllable: true, state: "on", value: 1 }],
    });
    expect(indicatorStates(view.host)[0]?.active).toBe("true");
    view.close();
  });
});
