// @vitest-environment jsdom

import { act, type ReactNode } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { flushSync } from "react-dom";
import App from "../App";
import type { AutomationTaskControl } from "@/lib/automationTask";
import type { GpioPhysicalLayout, SafeGpio } from "@/lib/types";
import { physicalLayoutFixture } from "@/lib/gpioPhysicalLayout.testFixtures";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const scrollIntoViewMock = vi.fn();
window.HTMLElement.prototype.scrollIntoView = scrollIntoViewMock;
const originalMatchMedia = window.matchMedia;

function setWideWorkspace(wide: boolean): void {
  Object.defineProperty(window, "matchMedia", {
    configurable: true,
    value: vi.fn().mockReturnValue({
      matches: wide,
      media: "(min-width: 1280px)",
      onchange: null,
      addEventListener: vi.fn(),
      removeEventListener: vi.fn(),
      addListener: vi.fn(),
      removeListener: vi.fn(),
      dispatchEvent: vi.fn(),
    }),
  });
}

const boardMocks = vi.hoisted(() => ({
  setSwitch: vi.fn((_name: string, _route: string) => Promise.resolve()),
  setPower: vi.fn((_name: string, _on: boolean) => Promise.resolve()),
  refresh: vi.fn(() => Promise.resolve()),
  rebootBoard: vi.fn(() => Promise.resolve()),
  rebootTransport: vi.fn(() => Promise.resolve()),
  useRealReboot: false,
  uptimeMs: null as number | null,
  enterBootloader: vi.fn(() => Promise.resolve()),
  restarting: false,
  restartTimedOut: false,
  connected: true,
  powerStateStale: false,
  powerUpdatedAt: Date.now(),
  lastVerifiedAt: null as number | null,
  physicalFixture: null as { layout: GpioPhysicalLayout; gpios: SafeGpio[] } | null,
  config: null as null | {
    readonly available: boolean;
    readonly reason: string;
    readonly savedCount: number;
    readonly pendingCount: number;
  },
  powerOutputs: [
    { name: "5v_out", controllable: true, state: "on", value: 1 },
    { name: "12v_out", controllable: true, state: "off", value: 0 },
    { name: "20v_out", controllable: true, state: "off", value: 0 },
  ],
}));

const persistentMocks = vi.hoisted(() => ({
  state: {
    config: null,
    error: null,
    loading: false,
    busy: null,
    supported: false,
    refresh: vi.fn(),
    save: vi.fn(),
    clear: vi.fn(),
  },
}));

const automationMocks = vi.hoisted(() => ({
  lastAcquire: null as boolean | null,
}));

const headerMocks = vi.hoisted(() => ({
  latest: null as null | {
    onSetVdd5v: (on: boolean) => Promise<void>;
    powerDisabled: boolean;
    powerStale: boolean;
    powerUpdatedAt: number | null;
    powerBusy: boolean;
  },
}));

vi.mock("@/hooks/useBoard", async () => {
  const { useBoardReboot } = await import("@/hooks/useBoardReboot");
  const { layoutForTestGpios } = await import("@/lib/gpioPhysicalLayout.testFixtures");
  return { useBoard: () => {
    const reboot = useBoardReboot(boardMocks.connected, boardMocks.uptimeMs);
    return {
    connected: boardMocks.connected,
    powerStateStale: boardMocks.powerStateStale,
    powerUpdatedAt: boardMocks.powerUpdatedAt,
    restarting: boardMocks.useRealReboot ? reboot.restarting : boardMocks.restarting,
    restartTimedOut: boardMocks.useRealReboot ? reboot.restartTimedOut : boardMocks.restartTimedOut,
    rebootBoard: boardMocks.useRealReboot ? reboot.rebootBoard : boardMocks.rebootBoard,
    lastVerifiedAt: boardMocks.lastVerifiedAt,
    loading: false,
    hasData: true,
    error: null,
    auto: true,
    live: true,
    gpioPhysicalLayout: boardMocks.physicalFixture?.layout ?? layoutForTestGpios([{ name: "GP10", pin: 10, note: "", value: 1, direction: "output" }]),
    gpioLayoutError: null,
    gpioLayoutLoading: false,
    snapshot: {
      config: boardMocks.config ?? undefined,
      powerOutputs: boardMocks.powerOutputs,
      adc: [],
      gpios: boardMocks.physicalFixture?.gpios ?? [{ name: "GP10", pin: 10, note: "", value: 1, direction: "output" }],
      gpioLayoutGroups: [{ name: "PORT", edge: "right", order: 0 }],
      switches: {},
    },
    captureState: "idle",
    captureProgress: null,
    captures: [],
    setAuto: vi.fn(),
    setLive: vi.fn(),
    refresh: boardMocks.refresh,
    setPower: boardMocks.setPower,
    readPower: vi.fn(),
    armCapture: vi.fn(),
    triggerCapture: vi.fn(),
    cancelCapture: vi.fn(),
    clearCaptures: vi.fn(),
    setSwitch: boardMocks.setSwitch,
    setGpio: vi.fn(),
    enterBootloader: boardMocks.enterBootloader,
    };
  } };
});

vi.mock("@/hooks/usePersistentConfig", () => ({
  usePersistentConfig: () => persistentMocks.state,
}));

vi.mock("@/lib/api", () => ({ rebootBoard: boardMocks.rebootTransport }));
vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({
    t: (key: string) => key,
  }),
}));

vi.mock("./StatusBar", () => ({
  StatusBar: (props: NonNullable<typeof headerMocks.latest>) => {
    headerMocks.latest = props;
    return null;
  },
}));
vi.mock("./PowerSparkline", () => ({ MeasurementSparkline: () => null }));
vi.mock("./SwitchCard", () => ({ SwitchCard: () => null }));
vi.mock("./BootCard", () => ({ BootCard: () => null }));
vi.mock("./NetworkCard", () => ({ NetworkCard: () => null }));
vi.mock("./GpioCard", () => ({
  GpioCard: ({ workspaceTabs, physicalLayout, gpios }: {
    readonly workspaceTabs?: ReactNode;
    readonly physicalLayout?: GpioPhysicalLayout | null;
    readonly gpios: readonly SafeGpio[];
  }) => (
    <div data-testid="gpio-card" data-layout-groups={physicalLayout?.groups.map((group) => group.name).join(",")}
      data-live-count={gpios.length} data-control-count={physicalLayout?.groups.flatMap((group) => group.cells).filter((cell) => cell.controllable).length}>{workspaceTabs}</div>
  ),
}));
vi.mock("./WatchdogCard", () => ({ WatchdogCard: () => <div data-testid="watchdog-card" /> }));
vi.mock("./StartupPowerAnalysis", () => ({ StartupPowerAnalysis: () => null }));
vi.mock("./TestAutomation", () => ({
  TestAutomation: ({
    focusMode,
    onFocusModeChange,
    workspaceTabs,
    taskControl,
  }: {
    focusMode?: boolean;
    onFocusModeChange?: (focused: boolean) => void;
    workspaceTabs?: ReactNode;
    taskControl: AutomationTaskControl;
  }) => (
    <div data-testid="test-automation" data-focused={String(focusMode)}>
      {workspaceTabs}
      <button
        type="button"
        data-testid="toggle-automation-focus"
        onClick={() => onFocusModeChange?.(!focusMode)}
      >
        toggle focus
      </button>
      <button
        type="button"
        data-testid="acquire-test-task"
        onClick={() => {
          automationMocks.lastAcquire = taskControl.acquire("test");
        }}
      >
        acquire test task
      </button>
    </div>
  ),
}));
vi.mock("./OtaCard", () => ({ OtaCard: () => null }));
vi.mock("./PersistentConfigCard", () => ({ PersistentConfigCard: () => null }));

type SerialCardMockProps = {
  readonly workspaceTabs?: ReactNode;
  readonly onConnectionChange?: (connections: { j4: boolean; state: "ready" }) => void;
};

vi.mock("./SerialCard", async () => {
  const { forwardRef, useEffect } = await import("react");
  return {
    SerialCard: forwardRef<unknown, SerialCardMockProps>(function SerialCardMock(
      { workspaceTabs, onConnectionChange },
      _ref
    ) {
      useEffect(() => {
        onConnectionChange?.({ j4: true, state: "ready" });
      }, [onConnectionChange]);
      return (
        <div data-testid="serial-card">
          {workspaceTabs}
        </div>
      );
    }),
  };
});

vi.mock("./PowerAnalysisWorkspace", () => ({
  PowerAnalysisWorkspace: ({ workspaceTabs }: { readonly workspaceTabs?: ReactNode }) => (
    <div data-testid="power-analysis">{workspaceTabs}</div>
  ),
}));

type LogicAnalyzerMockProps = {
  readonly boardGpios?: readonly { readonly name: string }[];
  readonly physicalLayout?: GpioPhysicalLayout | null;
  readonly workspaceTabs?: ReactNode;
};

vi.mock("./LogicAnalyzerCard", () => ({
  LogicAnalyzerCard: ({ boardGpios, physicalLayout, workspaceTabs }: LogicAnalyzerMockProps) => (
    <div data-testid="logic-analyzer" data-layout-groups={physicalLayout?.groups.map((group) => group.name).join(",")}>
      {workspaceTabs}
      {boardGpios?.map((gpio) => gpio.name).join(",")}
    </div>
  ),
}));

type AppView = {
  readonly host: HTMLDivElement;
  readonly rerender: () => void;
  readonly close: () => void;
};

let currentView: AppView | null = null;

function mountApp(): AppView {
  const host = document.createElement("div");
  document.body.append(host);
  const root: Root = createRoot(host);
  act(() => root.render(<App />));
  currentView = {
    host,
    rerender: () => act(() => root.render(<App />)),
    close: () => act(() => {
      root.unmount();
      host.remove();
    }),
  };
  return currentView;
}

function byId<T extends HTMLElement>(host: HTMLElement, id: string): T {
  const element = host.querySelector<T>(`#${id}`);
  if (!element) throw new TypeError(`Element not found: ${id}`);
  return element;
}

function byTestId<T extends HTMLElement>(host: HTMLElement, testId: string): T {
  const element = host.querySelector<T>(`[data-testid="${testId}"]`);
  if (!element) throw new TypeError(`Element not found: ${testId}`);
  return element;
}

function click(element: HTMLElement): void {
  act(() => element.click());
}

function press(element: HTMLElement, key: string): void {
  act(() => {
    element.dispatchEvent(new KeyboardEvent("keydown", { key, bubbles: true }));
  });
}

beforeEach(() => {
  vi.spyOn(window, "confirm").mockReturnValue(true);
});

afterEach(() => {
  currentView?.close();
  currentView = null;
  boardMocks.setSwitch.mockClear();
  boardMocks.setPower.mockReset();
  boardMocks.setPower.mockImplementation((_name: string, _on: boolean) => Promise.resolve());
  boardMocks.refresh.mockClear();
  boardMocks.rebootBoard.mockReset().mockResolvedValue(undefined);
  boardMocks.rebootTransport.mockReset().mockResolvedValue(undefined);
  boardMocks.useRealReboot = false;
  boardMocks.uptimeMs = null;
  boardMocks.enterBootloader.mockClear();
  boardMocks.restarting = false;
  boardMocks.restartTimedOut = false;
  boardMocks.connected = true;
  boardMocks.powerStateStale = false;
  boardMocks.lastVerifiedAt = null;
  boardMocks.physicalFixture = null;
  boardMocks.config = null;
  boardMocks.powerOutputs = [
    { name: "5v_out", controllable: true, state: "on", value: 1 },
    { name: "12v_out", controllable: true, state: "off", value: 0 },
    { name: "20v_out", controllable: true, state: "off", value: 0 },
  ];
  persistentMocks.state.config = null;
  persistentMocks.state.error = null;
  persistentMocks.state.loading = false;
  persistentMocks.state.busy = null;
  persistentMocks.state.supported = false;
  automationMocks.lastAcquire = null;
  headerMocks.latest = null;
  vi.useRealTimers();
  localStorage.clear();
  scrollIntoViewMock.mockClear();
  vi.restoreAllMocks();
  boardMocks.powerUpdatedAt = Date.now();
  Object.defineProperty(window, "matchMedia", {
    configurable: true,
    value: originalMatchMedia,
  });
});

describe("App workspace", () => {
  function drawerVdd5v(view: AppView): HTMLButtonElement {
    if (!document.body.querySelector("#hardware-controls")) click(byTestId(view.host, "open-hardware-controls"));
    return document.body.querySelector<HTMLButtonElement>('[data-power-rail="vdd_5v"] [role="switch"]')!;
  }

  it("uses the real drawer power switch with the same single confirmation and pending gate as the header", async () => {
    boardMocks.powerOutputs.push({ name: "vdd_5v", controllable: true, state: "off", value: 0 });
    let resolve!: () => void;
    boardMocks.setPower.mockReturnValue(new Promise<void>((done) => { resolve = done; }));
    const view = mountApp();
    const toggle = drawerVdd5v(view);
    act(() => { toggle.click(); toggle.click(); });
    expect(window.confirm).toHaveBeenCalledExactlyOnceWith("status.vdd5v.confirmOn");
    expect(boardMocks.setPower).toHaveBeenCalledExactlyOnceWith("vdd_5v", true);
    expect(toggle.disabled).toBe(true);
    expect(toggle.getAttribute("aria-busy")).toBe("true");
    expect(toggle.getAttribute("aria-checked")).toBe("false");
    expect(headerMocks.latest!.powerBusy).toBe(true);
    await act(async () => { await headerMocks.latest!.onSetVdd5v(true); });
    expect(window.confirm).toHaveBeenCalledTimes(1);
    click(byTestId(view.host, "acquire-test-task"));
    expect(automationMocks.lastAcquire).toBe(false);
    await act(async () => resolve());
    expect(toggle.disabled).toBe(false);
    expect(headerMocks.latest!.powerBusy).toBe(false);
  });

  it("keeps the real drawer read-only when its power snapshot expires", async () => {
    boardMocks.powerOutputs.push({ name: "vdd_5v", controllable: true, state: "off", value: 0 });
    const view = mountApp();
    boardMocks.powerStateStale = true;
    view.rerender();
    const toggle = drawerVdd5v(view);
    expect(toggle.disabled).toBe(true);
    act(() => toggle.click());
    expect(window.confirm).not.toHaveBeenCalled();
    expect(boardMocks.setPower).not.toHaveBeenCalled();
  });

  it("clears a drawer failure after a retry through the shared header action", async () => {
    boardMocks.powerOutputs.push({ name: "vdd_5v", controllable: true, state: "off", value: 0 });
    boardMocks.setPower.mockRejectedValueOnce(new Error("device busy"));
    const view = mountApp();
    const toggle = drawerVdd5v(view);
    await act(async () => toggle.click());
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain("device busy");
    await act(async () => { await headerMocks.latest!.onSetVdd5v(true); });
    expect(document.body.querySelector('[role="alert"]')).toBeNull();
    expect(boardMocks.setPower).toHaveBeenCalledTimes(2);
    expect(window.confirm).toHaveBeenCalledTimes(2);
  });

  it("cancels a drawer action without dispatching or acquiring a task", async () => {
    vi.mocked(window.confirm).mockReturnValue(false);
    boardMocks.powerOutputs.push({ name: "vdd_5v", controllable: true, state: "on", value: 1 });
    const view = mountApp();
    const toggle = drawerVdd5v(view);
    await act(async () => toggle.click());
    expect(window.confirm).toHaveBeenCalledExactlyOnceWith("status.vdd5v.confirmOff");
    expect(boardMocks.setPower).not.toHaveBeenCalled();
    expect(toggle.disabled).toBe(false);
    click(byTestId(view.host, "acquire-test-task"));
    expect(automationMocks.lastAcquire).toBe(true);
  });

  it.each(["expired", "disconnected", "state changed", "task acquired"])(
    "rechecks the latest state and lock after drawer confirmation: %s", async (change) => {
      const now = vi.spyOn(Date, "now").mockReturnValue(100_000);
      boardMocks.powerUpdatedAt = 100_000;
      boardMocks.powerOutputs.push({ name: "vdd_5v", controllable: true, state: "off", value: 0 });
      const view = mountApp();
      const toggle = drawerVdd5v(view);
      vi.mocked(window.confirm).mockImplementation(() => {
        if (change === "expired") now.mockReturnValue(130_000);
        else if (change === "disconnected") {
          boardMocks.connected = false;
          flushSync(() => view.rerender());
        } else if (change === "state changed") {
          boardMocks.powerOutputs = boardMocks.powerOutputs.map((output) => output.name === "vdd_5v"
            ? { ...output, state: "on", value: 1 } : output);
          flushSync(() => view.rerender());
        } else {
          // Acquire the real lock while the confirmation is open.
          click(byTestId(view.host, "acquire-test-task"));
        }
        return true;
      });
      await act(async () => toggle.click());
      expect(window.confirm).toHaveBeenCalledTimes(1);
      expect(boardMocks.setPower).not.toHaveBeenCalled();
      expect(document.body.querySelector('[role="alert"]')?.textContent)
        .toContain(change === "task acquired" ? "task.error.busy" : "status.vdd5v.unavailable");
      expect(headerMocks.latest!.powerBusy).toBe(false);
    },
  );

  it("routes the header toggle only to vdd_5v and holds the global lock until its request settles", async () => {
    boardMocks.powerOutputs.push({ name: "vdd_5v", controllable: true, state: "off", value: 0 });
    let resolve!: () => void;
    boardMocks.setPower.mockReturnValue(new Promise<void>((done) => { resolve = done; }));
    const view = mountApp();
    let request!: Promise<void>;
    await act(async () => { request = headerMocks.latest!.onSetVdd5v(true); });
    expect(boardMocks.setPower).toHaveBeenCalledExactlyOnceWith("vdd_5v", true);
    expect(headerMocks.latest!.powerDisabled).toBe(true);
    click(byTestId(view.host, "acquire-test-task"));
    expect(automationMocks.lastAcquire).toBe(false);
    await act(async () => { resolve(); await request; });
    expect(headerMocks.latest!.powerDisabled).toBe(false);
    expect(boardMocks.setSwitch).not.toHaveBeenCalled();
    expect(boardMocks.rebootBoard).not.toHaveBeenCalled();
    expect(byId(view.host, "workspace-tab-terminal").getAttribute("aria-selected")).toBe("true");
    click(byTestId(view.host, "acquire-test-task"));
    expect(automationMocks.lastAcquire).toBe(true);
  });

  it("checks the real task lock even when the header callback predates its acquisition", async () => {
    boardMocks.powerOutputs.push({ name: "vdd_5v", controllable: true, state: "off", value: 0 });
    const view = mountApp();
    const setHeaderPower = headerMocks.latest!.onSetVdd5v;
    click(byTestId(view.host, "acquire-test-task"));
    expect(headerMocks.latest!.powerDisabled).toBe(true);
    await expect(setHeaderPower(true)).rejects.toThrow("task.error.busy");
    expect(boardMocks.setPower).not.toHaveBeenCalled();
  });

  it("rejects stale or disconnected state after confirmation without a power request", async () => {
    boardMocks.powerOutputs.push({ name: "vdd_5v", controllable: true, state: "off", value: 0 });
    const view = mountApp();
    const setHeaderPower = headerMocks.latest!.onSetVdd5v;
    boardMocks.powerStateStale = true;
    view.rerender();
    expect(headerMocks.latest!.powerStale).toBe(true);
    await expect(setHeaderPower(true)).rejects.toThrow("status.vdd5v.unavailable");
    boardMocks.powerStateStale = false;
    boardMocks.connected = false;
    view.rerender();
    await expect(setHeaderPower(true)).rejects.toThrow("status.vdd5v.unavailable");
    expect(boardMocks.setPower).not.toHaveBeenCalled();
  });

  it("releases the header power lock after an error so another task can run", async () => {
    boardMocks.powerOutputs.push({ name: "vdd_5v", controllable: true, state: "on", value: 1 });
    boardMocks.setPower.mockRejectedValueOnce(new Error("hardware rejected request"));
    const view = mountApp();
    await act(async () => {
      await expect(headerMocks.latest!.onSetVdd5v(false)).rejects.toThrow("hardware rejected request");
    });
    expect(boardMocks.setPower).toHaveBeenCalledExactlyOnceWith("vdd_5v", false);
    expect(headerMocks.latest!.powerDisabled).toBe(false);
    click(byTestId(view.host, "acquire-test-task"));
    expect(automationMocks.lastAcquire).toBe(true);
  });

  it("passes the authoritative timestamp to the header and rejects an expired callback without waiting for a stale render", async () => {
    const now = vi.spyOn(Date, "now").mockReturnValue(100_000);
    boardMocks.powerUpdatedAt = 100_000;
    boardMocks.powerOutputs.push({ name: "vdd_5v", controllable: true, state: "off", value: 0 });
    mountApp();
    expect(headerMocks.latest!.powerUpdatedAt).toBe(100_000);
    expect(headerMocks.latest!.powerStale).toBe(false);
    const setHeaderPower = headerMocks.latest!.onSetVdd5v;
    now.mockReturnValue(131_000);
    await expect(setHeaderPower(true)).rejects.toThrow("status.vdd5v.unavailable");
    expect(boardMocks.setPower).not.toHaveBeenCalled();
  });

  it("releases the real reboot task after an uptime-confirmed restart with no offline transition", async () => {
    boardMocks.useRealReboot = true;
    boardMocks.uptimeMs = 100_000;
    vi.spyOn(window, "confirm").mockReturnValue(true);
    const view = mountApp();
    click(byId(view.host, "workspace-tab-configuration"));
    click(byTestId(view.host, "configuration-section-tab-firmware"));
    const restart = [...view.host.querySelectorAll<HTMLButtonElement>("button")].find((button) => button.textContent === "reboot.action")!;
    await act(async () => restart.click());
    expect(boardMocks.rebootTransport).toHaveBeenCalledTimes(1);
    expect(restart.disabled).toBe(true);
    click(byTestId(view.host, "acquire-test-task"));
    expect(automationMocks.lastAcquire).toBe(false);
    boardMocks.uptimeMs = 500;
    view.rerender();
    expect(boardMocks.connected).toBe(true);
    expect(restart.disabled).toBe(false);
    expect(restart.textContent).toBe("reboot.action");
    expect(view.host.textContent).not.toContain("reboot.reconnectTimeout");
    click(byTestId(view.host, "acquire-test-task"));
    expect(automationMocks.lastAcquire).toBe(true);
  });

  it("holds a distinct reboot task across maintenance navigation, hides expected outage errors and releases after reconnect", async () => {
    boardMocks.rebootBoard.mockImplementation(async () => { boardMocks.restarting = true; });
    const confirm = vi.spyOn(window, "confirm").mockReturnValue(true);
    const view = mountApp();
    click(byId(view.host, "workspace-tab-configuration"));
    click(byTestId(view.host, "configuration-section-tab-firmware"));
    const restart = [...view.host.querySelectorAll<HTMLButtonElement>("button")].find((button) => button.textContent === "reboot.action")!;
    await act(async () => restart.click());
    expect(confirm).toHaveBeenCalledWith("reboot.confirm");
    expect(boardMocks.rebootBoard).toHaveBeenCalledTimes(1);
    expect(boardMocks.enterBootloader).not.toHaveBeenCalled();
    expect(boardMocks.refresh).not.toHaveBeenCalled();
    click(byTestId(view.host, "configuration-section-tab-saved"));
    click(byTestId(view.host, "acquire-test-task"));
    expect(automationMocks.lastAcquire).toBe(false);
    boardMocks.connected = false;
    view.rerender();
    expect(view.host.textContent).toContain("reboot.wait");
    expect(view.host.textContent).not.toContain("banner.unreachable");
    boardMocks.connected = true;
    boardMocks.restarting = false;
    view.rerender();
    click(byTestId(view.host, "acquire-test-task"));
    expect(automationMocks.lastAcquire).toBe(true);
  });

  it("blocks the restart entry while another global hardware task holds the debugger", () => {
    const view = mountApp();
    const confirm = vi.spyOn(window, "confirm").mockReturnValue(true);
    click(byTestId(view.host, "acquire-test-task"));
    click(byId(view.host, "workspace-tab-configuration"));
    click(byTestId(view.host, "configuration-section-tab-firmware"));
    const restart = [...view.host.querySelectorAll<HTMLButtonElement>("button")].find((button) => button.textContent === "reboot.action")!;
    expect(restart.disabled).toBe(true);
    click(restart);
    expect(confirm).not.toHaveBeenCalled();
    expect(boardMocks.rebootBoard).not.toHaveBeenCalled();
  });

  it("checks the actual task lock again after confirmation before sending a restart", async () => {
    const view = mountApp();
    click(byId(view.host, "workspace-tab-configuration"));
    click(byTestId(view.host, "configuration-section-tab-firmware"));
    vi.spyOn(window, "confirm").mockImplementation(() => {
      byTestId<HTMLButtonElement>(view.host, "acquire-test-task").click();
      return true;
    });
    const restart = [...view.host.querySelectorAll<HTMLButtonElement>("button")].find((button) => button.textContent === "reboot.action")!;
    await act(async () => restart.click());
    expect(automationMocks.lastAcquire).toBe(true);
    expect(boardMocks.rebootBoard).not.toHaveBeenCalled();
    expect(view.host.querySelector('[role="alert"]')?.textContent).toContain("task.error.busy");
  });

  it("renders the terminal as the initial workspace and keeps maintenance separate", () => {
    const { host } = mountApp();

    const terminalTab = byId<HTMLButtonElement>(host, "workspace-tab-terminal");
    const logicTab = byId<HTMLButtonElement>(host, "workspace-tab-logicAnalyzer");
    const configurationTab = byId<HTMLButtonElement>(host, "workspace-tab-configuration");
    const terminalPanel = byId<HTMLDivElement>(host, "workspace-panel-terminal");
    const logicPanel = byId<HTMLDivElement>(host, "workspace-panel-logicAnalyzer");

    expect(host.querySelector("#workspace-tab-quickSetup")).toBeNull();
    expect(terminalTab.getAttribute("aria-selected")).toBe("true");
    expect(terminalTab.tabIndex).toBe(0);
    expect(logicTab.getAttribute("aria-selected")).toBe("false");
    expect(logicTab.tabIndex).toBe(-1);
    expect(configurationTab.getAttribute("aria-selected")).toBe("false");
    expect(terminalPanel.hidden).toBe(false);
    expect(logicPanel.hidden).toBe(true);
    expect(byTestId(host, "logic-analyzer").textContent).toBe("GP10");
    expect(byTestId(host, "logic-analyzer").getAttribute("data-layout-groups")).toBe("TEST");
  });

  it("switches to the Logic Analyzer panel when its tab is clicked", () => {
    const { host } = mountApp();

    click(byId<HTMLButtonElement>(host, "workspace-tab-logicAnalyzer"));

    const logicTab = byId<HTMLButtonElement>(host, "workspace-tab-logicAnalyzer");
    expect(logicTab.getAttribute("aria-selected")).toBe("true");
    expect(byId<HTMLDivElement>(host, "workspace-panel-terminal").hidden).toBe(true);
    expect(byId<HTMLDivElement>(host, "workspace-panel-logicAnalyzer").hidden).toBe(false);
  });

  it("restores focus and scrolls the clicked tab into view after the tablist remounts", () => {
    const { host } = mountApp();

    click(byId<HTMLButtonElement>(host, "workspace-tab-logicAnalyzer"));

    const logicTab = byId<HTMLButtonElement>(host, "workspace-tab-logicAnalyzer");
    expect(document.activeElement).toBe(logicTab);
    expect(scrollIntoViewMock).toHaveBeenCalledWith({ block: "nearest", inline: "nearest" });
  });

  it("keeps the selected workspace tab visible after the viewport changes", () => {
    const { host } = mountApp();
    click(byId<HTMLButtonElement>(host, "workspace-tab-automation"));
    scrollIntoViewMock.mockClear();

    act(() => window.dispatchEvent(new Event("resize")));

    expect(scrollIntoViewMock).toHaveBeenCalledOnce();
    expect(scrollIntoViewMock).toHaveBeenCalledWith({ block: "nearest", inline: "nearest" });
  });

  it("moves focus and selection with ArrowRight keyboard navigation", () => {
    const { host } = mountApp();
    const terminalTab = byId<HTMLButtonElement>(host, "workspace-tab-terminal");
    act(() => terminalTab.focus());

    press(terminalTab, "ArrowRight");

    const powerTab = byId<HTMLButtonElement>(host, "workspace-tab-powerAnalysis");
    expect(document.activeElement).toBe(powerTab);
    expect(powerTab.getAttribute("aria-selected")).toBe("true");
    expect(powerTab.tabIndex).toBe(0);
  });

  it("keeps workspace tabs on a fixed horizontal scan line instead of wrapping", () => {
    // Given: the rendered global workspace navigation
    const { host } = mountApp();

    // When: locating the tablist and its scroll container
    const tablist = host.querySelector('[role="tablist"][aria-label="workspace.tabs"]');
    const scroller = tablist?.parentElement;

    // Then: the outer wrapper owns horizontal scrolling while the tablist keeps
    // a fixed single-line width, so narrow viewports scroll instead of wrapping
    expect(scroller?.className).toContain("overflow-x-auto");
    expect(tablist?.className).toContain("min-w-max");
    expect(tablist?.className).not.toContain("flex-wrap");
  });

  it("renders exactly five workspace tabs with no GPIO workspace", () => {
    const { host } = mountApp();

    const tablist = host.querySelector('[role="tablist"][aria-label="workspace.tabs"]');
    expect(tablist?.querySelectorAll('[role="tab"]')).toHaveLength(5);
    expect(host.querySelector("#workspace-tab-gpio")).toBeNull();
    expect(host.querySelector("#workspace-panel-gpio")).toBeNull();
    expect(host.querySelector("#workspace-tab-configuration")).not.toBeNull();
  });

  it("moves End to the Configuration tab and wraps ArrowRight back to Terminal", () => {
    const { host } = mountApp();
    const automationTab = byId<HTMLButtonElement>(host, "workspace-tab-automation");
    act(() => automationTab.focus());

    press(automationTab, "End");

    const configurationTab = byId<HTMLButtonElement>(host, "workspace-tab-configuration");
    expect(document.activeElement).toBe(configurationTab);
    expect(configurationTab.getAttribute("aria-selected")).toBe("true");

    press(configurationTab, "ArrowRight");

    const terminalTab = byId<HTMLButtonElement>(host, "workspace-tab-terminal");
    expect(document.activeElement).toBe(terminalTab);
    expect(terminalTab.getAttribute("aria-selected")).toBe("true");
  });

  it("keeps automation full-width and opens device controls on demand", () => {
    const { host } = mountApp();

    click(byId<HTMLButtonElement>(host, "workspace-tab-automation"));

    expect(byTestId(host, "workspace-layout").classList.contains("grid-cols-1")).toBe(true);
    expect(host.querySelector('[data-testid="hardware-sidebar"]')).toBeNull();
    expect(byTestId(host, "test-automation").dataset.focused).toBe("true");

    click(byTestId(host, "toggle-automation-focus"));

    expect(document.body.querySelector('dialog[aria-labelledby="hardware-controls-title"]')).not.toBeNull();
    expect(byTestId(host, "test-automation").dataset.focused).toBe("false");
  });

  it("opens device controls as a mobile dialog without inserting them before the workflow", () => {
    setWideWorkspace(false);
    const { host } = mountApp();
    click(byId<HTMLButtonElement>(host, "workspace-tab-automation"));
    const toggle = byTestId<HTMLButtonElement>(host, "toggle-automation-focus");
    act(() => toggle.focus());

    click(toggle);

    const dialog = document.body.querySelector<HTMLDialogElement>('dialog[aria-labelledby="hardware-controls-title"]');
    expect(dialog).not.toBeNull();
    expect(host.querySelector('[data-testid="hardware-sidebar"]')).toBeNull();
    expect(document.body.style.overflow).toBe("hidden");

    const closeButton = document.body.querySelector<HTMLButtonElement>('[aria-label="test.hardware.close"]');
    if (!closeButton) throw new TypeError("Mobile device controls close button not found");
    click(closeButton);

    expect(document.body.querySelector('dialog[aria-labelledby="hardware-controls-title"]')).toBeNull();
    expect(byTestId(host, "test-automation").dataset.focused).toBe("true");
    expect(document.activeElement).toBe(toggle);
  });

  it("opens configuration and maintenance as its own workspace", () => {
    const { host } = mountApp();
    click(byId<HTMLButtonElement>(host, "workspace-tab-configuration"));

    expect(byId<HTMLButtonElement>(host, "workspace-tab-configuration").getAttribute("aria-selected")).toBe("true");
    expect(byId<HTMLDivElement>(host, "workspace-panel-configuration").hidden).toBe(false);
    expect(byTestId(host, "configuration-workspace")).not.toBeNull();
    expect(document.body.querySelector('dialog[aria-labelledby="hardware-controls-title"]')).toBeNull();
  });

  it("keeps task workspaces full-width without inserting hardware cards", () => {
    setWideWorkspace(false);
    const { host } = mountApp();
    click(byId<HTMLButtonElement>(host, "workspace-tab-terminal"));

    expect(byTestId(host, "workspace-layout").classList.contains("grid-cols-1")).toBe(true);
    expect(host.querySelector('[data-testid="hardware-sidebar"]')).toBeNull();
    expect(byTestId(host, "workspace-main")).not.toBeNull();
  });

  it("opens the complete hardware drawer from the global navigation", () => {
    const { host } = mountApp();

    click(byTestId(host, "open-hardware-controls"));

    const dialog = document.body.querySelector<HTMLDialogElement>('dialog[aria-labelledby="hardware-controls-title"]');
    expect(dialog).not.toBeNull();
    expect(dialog?.classList.contains("relative")).toBe(true);
    expect(dialog?.querySelector('[data-testid="hardware-section-tab-power"]')?.getAttribute("aria-selected")).toBe("true");
    expect(dialog?.querySelector('[data-testid="hardware-section-panel-power"]')).not.toBeNull();
    expect(dialog?.querySelector('[data-testid="target-recovery-card"]')).toBeNull();
    const anchors = dialog?.querySelector('[data-testid="hardware-control-anchors"]');
    expect(anchors).not.toBeNull();
    expect(anchors?.querySelector('[data-testid="hardware-anchor-recovery"]')).toBeNull();
    scrollIntoViewMock.mockClear();
    click(byTestId<HTMLButtonElement>(anchors as HTMLElement, "hardware-anchor-routing"));
    expect(scrollIntoViewMock).toHaveBeenCalledWith({ behavior: "smooth", block: "start" });
    expect(dialog?.querySelector('[data-testid="hardware-section-panel-io"]')).toBeNull();
  });

  it("closes the hardware drawer from the backdrop without closing on drawer content", () => {
    const { host } = mountApp();
    const opener = byTestId<HTMLButtonElement>(host, "open-hardware-controls");
    act(() => opener.focus());
    click(opener);

    const dialog = document.body.querySelector<HTMLDialogElement>(
      'dialog[aria-labelledby="hardware-controls-title"]'
    );
    if (!dialog) throw new TypeError("Hardware controls dialog not found");
    const backdrop = byTestId<HTMLDivElement>(document.body, "hardware-controls-backdrop");

    click(dialog);
    expect(document.body.querySelector('dialog[aria-labelledby="hardware-controls-title"]')).toBe(dialog);

    click(backdrop);
    expect(document.body.querySelector('dialog[aria-labelledby="hardware-controls-title"]')).toBeNull();
    expect(document.body.style.overflow).toBe("");
    expect(document.activeElement).toBe(opener);
  });

  it("shows one hardware task group at a time", () => {
    const { host } = mountApp();
    click(byTestId(host, "open-hardware-controls"));
    const dialog = document.body.querySelector<HTMLDialogElement>('dialog[aria-labelledby="hardware-controls-title"]');
    if (!dialog) throw new TypeError("Hardware controls dialog not found");

    click(byTestId<HTMLButtonElement>(dialog, "hardware-section-tab-io"));
    expect(byTestId(dialog, "hardware-section-tab-io").getAttribute("aria-selected")).toBe("true");
    expect(byTestId(dialog, "hardware-section-panel-io")).not.toBeNull();
    expect(dialog.querySelector('[data-testid="hardware-section-panel-power"]')).toBeNull();
    expect(dialog.querySelector('[data-testid="hardware-section-tab-recovery"]')).toBeNull();
    expect(dialog.querySelector('[data-testid="hardware-section-tab-firmware"]')).toBeNull();
  });

  it("moves hardware tab focus and selection with arrows, wraps, and supports Home/End", () => {
    const view = mountApp();
    click(byTestId(view.host, "open-hardware-controls"));
    const power = byTestId<HTMLButtonElement>(document.body, "hardware-section-tab-power");
    const io = byTestId<HTMLButtonElement>(document.body, "hardware-section-tab-io");
    act(() => power.focus());
    expect(power.tabIndex).toBe(0);
    expect(io.tabIndex).toBe(-1);
    for (const [from, key, to] of [[power, "ArrowRight", io], [io, "ArrowRight", power],
      [power, "ArrowLeft", io], [io, "Home", power], [power, "End", io]] as const) {
      press(from, key);
      expect(document.activeElement).toBe(to);
      expect(to.getAttribute("aria-selected")).toBe("true");
      expect(to.tabIndex).toBe(0);
      expect((to === power ? io : power).tabIndex).toBe(-1);
      expect(document.getElementById(to.getAttribute("aria-controls")!)).not.toBeNull();
    }
    expect(localStorage.getItem("linkr-hardware-controls-section")).toBe("io");
  });

  it("places the sole physical GPIO card immediately before Watchdog in the IO drawer", () => {
    boardMocks.physicalFixture = physicalLayoutFixture();
    const { host } = mountApp();
    click(byTestId(host, "open-hardware-controls"));
    const dialog = document.body.querySelector<HTMLDialogElement>('dialog[aria-labelledby="hardware-controls-title"]');
    if (!dialog) throw new TypeError("Hardware controls dialog not found");

    click(byTestId<HTMLButtonElement>(dialog, "hardware-section-tab-io"));

    expect(dialog.querySelector('[data-testid="gpio-card"]')).not.toBeNull();
    expect(dialog.querySelector('[data-testid="gpio-card"]')?.getAttribute("data-layout-groups")).toBe(boardMocks.physicalFixture.layout.groups.map((group) => group.name).join(","));
    expect(dialog.querySelector('[data-testid="gpio-card"]')?.getAttribute("data-live-count")).toBe("11");
    expect(dialog.querySelector('[data-testid="gpio-card"]')?.getAttribute("data-control-count")).toBe("11");
    expect(dialog.querySelector('[data-testid="watchdog-card"]')).not.toBeNull();
    expect(dialog.querySelector('[data-testid="gpio-controls-anchor"]')).not.toBeNull();
    const controls = byTestId(dialog, "gpio-controls-anchor");
    const watchdog = byTestId(dialog, "watchdog-card");
    expect(controls.nextElementSibling).toBe(watchdog);
    expect(controls.parentElement).toBe(watchdog.parentElement);
    expect([...controls.parentElement!.children]).toEqual([controls, watchdog]);
    expect(controls.querySelector('[data-testid="gpio-card"]')).not.toBeNull();
    expect(dialog.querySelectorAll('[data-testid="gpio-card"]')).toHaveLength(1);
    expect(dialog.querySelector('[data-testid="gpio-reference-column"]')).toBeNull();
  });

  it("closes the hardware drawer on an exact backdrop click but not on inside clicks", () => {
    const { host } = mountApp();
    click(byTestId(host, "open-hardware-controls"));
    const dialog = document.body.querySelector<HTMLDialogElement>('dialog[aria-labelledby="hardware-controls-title"]');
    if (!dialog) throw new TypeError("Hardware controls dialog not found");

    act(() => {
      dialog.dispatchEvent(new MouseEvent("click", { bubbles: true, cancelable: true }));
    });
    expect(document.body.querySelector('dialog[aria-labelledby="hardware-controls-title"]')).not.toBeNull();

    const backdrop = byTestId<HTMLDivElement>(document.body, "hardware-controls-backdrop");
    act(() => {
      backdrop.dispatchEvent(new MouseEvent("click", { bubbles: true, cancelable: true }));
    });
    expect(document.body.querySelector('dialog[aria-labelledby="hardware-controls-title"]')).toBeNull();
  });

  it("restores the persisted hardware section from localStorage on open", () => {
    localStorage.setItem("linkr-hardware-controls-section", "io");
    const { host } = mountApp();

    click(byTestId(host, "open-hardware-controls"));

    const dialog = document.body.querySelector<HTMLDialogElement>('dialog[aria-labelledby="hardware-controls-title"]');
    if (!dialog) throw new TypeError("Hardware controls dialog not found");
    expect(byTestId(dialog, "hardware-section-tab-io").getAttribute("aria-selected")).toBe("true");
    expect(dialog.querySelector('[data-testid="hardware-section-panel-io"]')).not.toBeNull();
  });

  it("keeps the selected hardware section when the drawer is closed and reopened", () => {
    const { host } = mountApp();
    click(byTestId(host, "open-hardware-controls"));
    const firstDialog = document.body.querySelector<HTMLDialogElement>('dialog[aria-labelledby="hardware-controls-title"]');
    if (!firstDialog) throw new TypeError("Hardware controls dialog not found");
    click(byTestId<HTMLButtonElement>(firstDialog, "hardware-section-tab-io"));
    click(firstDialog.querySelector<HTMLButtonElement>('[aria-label="test.hardware.close"]') as HTMLButtonElement);

    click(byTestId(host, "open-hardware-controls"));

    const dialog = document.body.querySelector<HTMLDialogElement>('dialog[aria-labelledby="hardware-controls-title"]');
    if (!dialog) throw new TypeError("Hardware controls dialog not found");
    expect(byTestId(dialog, "hardware-section-tab-io").getAttribute("aria-selected")).toBe("true");
    expect(localStorage.getItem("linkr-hardware-controls-section")).toBe("io");
  });

  it("defaults to the power section when the persisted value is invalid", () => {
    localStorage.setItem("linkr-hardware-controls-section", "bogus");
    const { host } = mountApp();

    click(byTestId(host, "open-hardware-controls"));

    const dialog = document.body.querySelector<HTMLDialogElement>('dialog[aria-labelledby="hardware-controls-title"]');
    if (!dialog) throw new TypeError("Hardware controls dialog not found");
    expect(byTestId(dialog, "hardware-section-tab-power").getAttribute("aria-selected")).toBe("true");
  });

  it("survives localStorage read and write failures", () => {
    const getItem = vi.spyOn(Storage.prototype, "getItem").mockImplementation(() => {
      throw new Error("denied");
    });
    const { host } = mountApp();

    click(byTestId(host, "open-hardware-controls"));

    const dialog = document.body.querySelector<HTMLDialogElement>('dialog[aria-labelledby="hardware-controls-title"]');
    if (!dialog) throw new TypeError("Hardware controls dialog not found");
    expect(byTestId(dialog, "hardware-section-tab-power").getAttribute("aria-selected")).toBe("true");
    getItem.mockRestore();

    const setItem = vi.spyOn(Storage.prototype, "setItem").mockImplementation(() => {
      throw new Error("denied");
    });
    click(byTestId<HTMLButtonElement>(dialog, "hardware-section-tab-io"));
    expect(byTestId(dialog, "hardware-section-tab-io").getAttribute("aria-selected")).toBe("true");
    expect(dialog.querySelector('[data-testid="hardware-section-panel-io"]')).not.toBeNull();
    setItem.mockRestore();
  });

  it("preserves the restored hardware section when the drawer opens from automation", () => {
    localStorage.setItem("linkr-hardware-controls-section", "io");
    const { host } = mountApp();
    click(byId<HTMLButtonElement>(host, "workspace-tab-automation"));

    click(byTestId(host, "toggle-automation-focus"));

    const dialog = document.body.querySelector<HTMLDialogElement>('dialog[aria-labelledby="hardware-controls-title"]');
    if (!dialog) throw new TypeError("Hardware controls dialog not found");
    expect(byTestId(dialog, "hardware-section-tab-io").getAttribute("aria-selected")).toBe("true");
  });
});
