// @vitest-environment jsdom

import { act, type ReactNode } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import App from "../App";
import type { AutomationTaskControl } from "@/lib/automationTask";

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
  connected: true,
  lastVerifiedAt: null as number | null,
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

vi.mock("@/hooks/useBoard", () => ({
  useBoard: () => ({
    connected: boardMocks.connected,
    lastVerifiedAt: boardMocks.lastVerifiedAt,
    loading: false,
    hasData: true,
    error: null,
    auto: true,
    live: true,
    snapshot: {
      config: boardMocks.config ?? undefined,
      powerOutputs: boardMocks.powerOutputs,
      adc: [],
      gpios: [{ name: "GP10", pin: 10, note: "", value: 1, direction: "output" }],
      switches: { vin: { route: "3.3v" } },
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
    enterBootloader: vi.fn(),
  }),
}));

vi.mock("@/hooks/usePersistentConfig", () => ({
  usePersistentConfig: () => persistentMocks.state,
}));

vi.mock("@/lib/api", () => ({}));
vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({
    t: (key: string) => key,
  }),
}));

vi.mock("./StatusBar", () => ({ StatusBar: () => null }));
vi.mock("./PowerCard", () => ({ PowerCard: () => null }));
vi.mock("./SwitchCard", () => ({ SwitchCard: () => null }));
vi.mock("./BootCard", () => ({ BootCard: () => null }));
vi.mock("./GpioCard", () => ({
  GpioCard: ({ workspaceTabs }: { readonly workspaceTabs?: ReactNode }) => (
    <div data-testid="gpio-card">{workspaceTabs}</div>
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
  readonly vinRoute?: string;
  readonly onSetVin: (route: "1.8v" | "3.3v") => Promise<void>;
  readonly workspaceTabs?: ReactNode;
  readonly onConnectionChange?: (connections: { uart0: boolean; uart1: boolean }) => void;
};

vi.mock("./SerialCard", async () => {
  const { forwardRef, useEffect } = await import("react");
  return {
    SerialCard: forwardRef<unknown, SerialCardMockProps>(function SerialCardMock(
      { vinRoute, onSetVin, workspaceTabs, onConnectionChange },
      _ref
    ) {
      useEffect(() => {
        onConnectionChange?.({ uart0: true, uart1: false });
      }, [onConnectionChange]);
      return (
        <div data-testid="serial-card" data-vin-route={vinRoute}>
          {workspaceTabs}
          <button type="button" data-testid="set-vin" onClick={() => void onSetVin("1.8v")}>
            set-vin
          </button>
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
  readonly workspaceTabs?: ReactNode;
};

vi.mock("./LogicAnalyzerCard", () => ({
  LogicAnalyzerCard: ({ boardGpios, workspaceTabs }: LogicAnalyzerMockProps) => (
    <div data-testid="logic-analyzer">
      {workspaceTabs}
      {boardGpios?.map((gpio) => gpio.name).join(",")}
    </div>
  ),
}));

type AppView = {
  readonly host: HTMLDivElement;
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

afterEach(() => {
  currentView?.close();
  currentView = null;
  boardMocks.setSwitch.mockClear();
  boardMocks.setPower.mockReset();
  boardMocks.setPower.mockImplementation((_name: string, _on: boolean) => Promise.resolve());
  boardMocks.refresh.mockClear();
  boardMocks.connected = true;
  boardMocks.lastVerifiedAt = null;
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
  vi.useRealTimers();
  localStorage.clear();
  scrollIntoViewMock.mockClear();
  Object.defineProperty(window, "matchMedia", {
    configurable: true,
    value: originalMatchMedia,
  });
});

describe("App workspace", () => {
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
    expect(byTestId(host, "serial-card").dataset.vinRoute).toBe("3.3v");
    expect(byTestId(host, "logic-analyzer").textContent).toBe("GP10");
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

  it("keeps the serial VIN callback mapped to the board vin switch", () => {
    const { host } = mountApp();
    const setVin = byTestId(host, "set-vin");

    click(setVin);

    expect(boardMocks.setSwitch).toHaveBeenCalledOnce();
    expect(boardMocks.setSwitch).toHaveBeenCalledWith("vin", "1.8v");
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

  it("keeps GPIO and watchdog controls in the hardware drawer io section", () => {
    const { host } = mountApp();
    click(byTestId(host, "open-hardware-controls"));
    const dialog = document.body.querySelector<HTMLDialogElement>('dialog[aria-labelledby="hardware-controls-title"]');
    if (!dialog) throw new TypeError("Hardware controls dialog not found");

    click(byTestId<HTMLButtonElement>(dialog, "hardware-section-tab-io"));

    expect(dialog.querySelector('[data-testid="gpio-card"]')).not.toBeNull();
    expect(dialog.querySelector('[data-testid="watchdog-card"]')).not.toBeNull();
    expect(dialog.querySelector('[data-testid="gpio-controls-anchor"]')).not.toBeNull();
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
