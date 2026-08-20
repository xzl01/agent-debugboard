import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import type { SerialAutomationHandle } from "./SerialCard";
import { StartupPowerAnalysis } from "./StartupPowerAnalysis";
import { PowerAnalysisWorkspace } from "./PowerAnalysisWorkspace";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({ t: (key: string) => key }),
}));

describe("StartupPowerAnalysis workspace", () => {
  afterEach(() => {
    document.body.replaceChildren();
    localStorage.clear();
    vi.restoreAllMocks();
  });

  it("exposes live capture and startup analysis as explicit sibling modes", () => {
    const host = document.createElement("div");
    document.body.append(host);
    const root = createRoot(host);
    const serialRef = {
      current: {
        isConnected: () => false,
        connectedChannels: () => [],
        clear: vi.fn(),
        write: vi.fn(() => Promise.resolve()),
        setAutomationActive: vi.fn(),
        subscribe: vi.fn(() => () => {}),
      } satisfies SerialAutomationHandle,
    };

    act(() => root.render(
      <PowerAnalysisWorkspace
        outputs={[{ name: "5v_out", controllable: true, state: "on", value: 1 }]}
        gpios={[]}
        captureState="idle"
        captureProgress={null}
        captures={[]}
        serialRef={serialRef}
        onSetPower={vi.fn(() => Promise.resolve())}
        onReadPower={vi.fn(() => Promise.resolve({ state: "on", currentUa: 0 }))}
        onArmCapture={vi.fn(() => Promise.resolve())}
        onTriggerCapture={vi.fn()}
        onStopCapture={vi.fn()}
        onCancelCapture={vi.fn()}
        onClearCaptures={vi.fn()}
        taskControl={{ owner: null, acquire: () => true, release: vi.fn() }}
      />
    ));

    const tabs = [...host.querySelectorAll<HTMLButtonElement>('[data-testid="power-analysis-mode-switch"] [role="tab"]')];
    expect(tabs).toHaveLength(2);
    expect(tabs[0]?.textContent).toContain("powerAnalysis.capture");
    expect(tabs[0]?.textContent).toContain("powerAnalysis.captureSummary");
    expect(tabs[1]?.textContent).toContain("powerAnalysis.startup");
    expect(tabs[1]?.textContent).toContain("powerAnalysis.startupSummary");
    expect(tabs[0]?.getAttribute("aria-selected")).toBe("true");

    act(() => tabs[1]?.click());
    expect(tabs[1]?.getAttribute("aria-selected")).toBe("true");
    expect(host.querySelector('#power-analysis-panel-startup')).not.toBeNull();

    act(() => root.unmount());
  });

  it("keeps the execution evidence separate from the editable setup", () => {
    const host = document.createElement("div");
    document.body.append(host);
    const root = createRoot(host);
    const serialRef = {
      current: {
        isConnected: () => false,
        connectedChannels: () => [],
        clear: vi.fn(),
        write: vi.fn(() => Promise.resolve()),
        setAutomationActive: vi.fn(),
        subscribe: vi.fn(() => () => {}),
      } satisfies SerialAutomationHandle,
    };

    act(() => root.render(
      <StartupPowerAnalysis
        outputs={[{ name: "5v_out", controllable: true, state: "off", value: 0 }]}
        captureState="idle"
        captures={[]}
        serialRef={serialRef}
        onSetPower={vi.fn(() => Promise.resolve())}
        onReadPower={vi.fn(() => Promise.resolve({ state: "off", currentUa: 0 }))}
        onArmCapture={vi.fn(() => Promise.resolve())}
        onStopCapture={vi.fn()}
        onCancelCapture={vi.fn()}
        taskControl={{ owner: null, acquire: () => true, release: vi.fn() }}
      />
    ));

    expect(host.querySelector("main")).not.toBeNull();
    expect(host.querySelector("aside")).not.toBeNull();
    expect(host.textContent).toContain("startup.empty.title");
    expect(host.textContent).not.toContain("startup.phase.powering_off");
    expect(host.textContent).not.toContain("startup.phase.capturing");
    expect(host.textContent).toContain("startup.automation.title");
    expect(host.textContent).toContain("startup.start");

    act(() => root.unmount());
  });

  it("locks UART evidence even without auto-login and releases only after cancel and power restore", async () => {
    const host = document.createElement("div");
    document.body.append(host);
    const root = createRoot(host);
    let resolvePowerOff = () => {};
    let resolveRestore = () => {};
    const powerOff = new Promise<void>((resolve) => { resolvePowerOff = resolve; });
    const restore = new Promise<void>((resolve) => { resolveRestore = resolve; });
    let restoringPower = false;
    const onSetPower = vi.fn((_rail: string, on: boolean) => {
      restoringPower = on;
      return on ? restore : powerOff;
    });
    const onCancelCapture = vi.fn();
    const release = vi.fn();
    const setAutomationActive = vi.fn();
    const serialRef = {
      current: {
        isConnected: () => true,
        connectedChannels: () => ["uart0" as const],
        clear: vi.fn(),
        write: vi.fn(() => Promise.resolve()),
        setAutomationActive,
        subscribe: vi.fn(() => () => {}),
      } satisfies SerialAutomationHandle,
    };
    const render = (captureState: "idle" | "recording") => (
      <StartupPowerAnalysis
        outputs={[{ name: "5v_out", controllable: true, state: "on", value: 1 }]}
        captureState={captureState}
        captures={[]}
        serialRef={serialRef}
        onSetPower={onSetPower}
        onReadPower={vi.fn(() => Promise.resolve({ state: restoringPower ? "on" : "off", currentUa: 0 }))}
        onArmCapture={vi.fn(() => Promise.resolve())}
        onStopCapture={vi.fn()}
        onCancelCapture={onCancelCapture}
        taskControl={{ owner: null, acquire: () => true, release }}
      />
    );
    vi.spyOn(window, "confirm").mockReturnValue(true);
    act(() => root.render(render("idle")));

    const start = [...host.querySelectorAll("button")].find((item) => item.textContent?.includes("startup.start"));
    expect(start).toBeDefined();
    act(() => start?.click());
    expect(setAutomationActive).toHaveBeenCalledWith(true, "uart0");

    act(() => root.render(render("recording")));
    act(() => root.unmount());
    expect(onCancelCapture).toHaveBeenCalledTimes(1);
    expect(onSetPower).toHaveBeenCalledWith("5v_out", true);
    expect(release).not.toHaveBeenCalled();

    await act(async () => {
      resolveRestore();
      resolvePowerOff();
      await Promise.resolve();
    });
    expect(setAutomationActive).toHaveBeenLastCalledWith(false, "uart0");
    expect(release).toHaveBeenCalledWith("startup");
  });

  it("keeps startup mounted by disabling mode changes while its task lock is active", () => {
    const host = document.createElement("div");
    document.body.append(host);
    const root = createRoot(host);
    const serialRef = {
      current: {
        isConnected: () => false,
        connectedChannels: () => [],
        clear: vi.fn(),
        write: vi.fn(() => Promise.resolve()),
        setAutomationActive: vi.fn(),
        subscribe: vi.fn(() => () => {}),
      } satisfies SerialAutomationHandle,
    };
    const render = (owner: "startup" | null) => (
      <PowerAnalysisWorkspace
        outputs={[{ name: "5v_out", controllable: true, state: "on", value: 1 }]}
        gpios={[]}
        captureState="idle"
        captureProgress={null}
        captures={[]}
        serialRef={serialRef}
        onSetPower={vi.fn(() => Promise.resolve())}
        onReadPower={vi.fn(() => Promise.resolve({ state: "on", currentUa: 0 }))}
        onArmCapture={vi.fn(() => Promise.resolve())}
        onTriggerCapture={vi.fn()}
        onStopCapture={vi.fn()}
        onCancelCapture={vi.fn()}
        onClearCaptures={vi.fn()}
        taskControl={{ owner, acquire: () => true, release: vi.fn() }}
      />
    );
    act(() => root.render(render(null)));
    const startupTab = [...host.querySelectorAll('[role="tab"]')].find((tab) =>
      tab.textContent?.includes("powerAnalysis.startup")
    ) as HTMLButtonElement | undefined;
    act(() => startupTab?.click());
    act(() => root.render(render("startup")));

    const captureTab = [...host.querySelectorAll('[role="tab"]')].find((tab) =>
      tab.textContent?.includes("powerAnalysis.capture")
    ) as HTMLButtonElement | undefined;
    expect(captureTab?.disabled).toBe(true);
    expect(host.textContent).toContain("startup.title");
    act(() => root.unmount());
  });
});
