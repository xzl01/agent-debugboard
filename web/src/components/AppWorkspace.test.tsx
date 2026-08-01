import { act, type ReactNode } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import App from "../App";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const scrollIntoViewMock = vi.fn();
window.HTMLElement.prototype.scrollIntoView = scrollIntoViewMock;

const boardMocks = vi.hoisted(() => ({
  setSwitch: vi.fn((_name: string, _route: string) => Promise.resolve()),
}));

vi.mock("@/hooks/useBoard", () => ({
  useBoard: () => ({
    connected: true,
    loading: false,
    hasData: true,
    error: null,
    auto: true,
    live: true,
    snapshot: {
      config: undefined,
      powerOutputs: [],
      adc: [],
      gpios: [{ name: "GP10" }],
      switches: { vin: { route: "3.3v" } },
    },
    captureState: "idle",
    captureProgress: null,
    captures: [],
    setAuto: vi.fn(),
    setLive: vi.fn(),
    refresh: vi.fn(),
    setPower: vi.fn(),
    readPower: vi.fn(),
    armCapture: vi.fn(),
    triggerCapture: vi.fn(),
    cancelCapture: vi.fn(),
    clearCaptures: vi.fn(),
    setSwitch: boardMocks.setSwitch,
    enterBootloader: vi.fn(),
  }),
}));

vi.mock("@/hooks/usePersistentConfig", () => ({
  usePersistentConfig: () => ({}),
}));

vi.mock("@/lib/api", () => ({ apiEndpoint: () => "/api/v1" }));
vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({ t: (key: string) => key }),
}));

vi.mock("./StatusBar", () => ({ StatusBar: () => null }));
vi.mock("./PowerCard", () => ({ PowerCard: () => null }));
vi.mock("./SwitchCard", () => ({ SwitchCard: () => null }));
vi.mock("./BootCard", () => ({ BootCard: () => null }));
vi.mock("./StartupPowerAnalysis", () => ({ StartupPowerAnalysis: () => null }));
vi.mock("./TestAutomation", () => ({ TestAutomation: () => null }));
vi.mock("./OtaCard", () => ({ OtaCard: () => null }));
vi.mock("./PersistentConfigCard", () => ({ PersistentConfigCard: () => null }));

type SerialCardMockProps = {
  readonly vinRoute?: string;
  readonly onSetVin: (route: "1.8v" | "3.3v") => Promise<void>;
  readonly workspaceTabs?: ReactNode;
};

vi.mock("./SerialCard", async () => {
  const { forwardRef } = await import("react");
  return {
    SerialCard: forwardRef<unknown, SerialCardMockProps>(function SerialCardMock(
      { vinRoute, onSetVin, workspaceTabs },
      _ref
    ) {
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
  scrollIntoViewMock.mockClear();
});

describe("App workspace", () => {
  it("renders the terminal tab and panel as the initial selected workspace", () => {
    const { host } = mountApp();

    const terminalTab = byId<HTMLButtonElement>(host, "workspace-tab-terminal");
    const logicTab = byId<HTMLButtonElement>(host, "workspace-tab-logicAnalyzer");
    const terminalPanel = byId<HTMLDivElement>(host, "workspace-panel-terminal");
    const logicPanel = byId<HTMLDivElement>(host, "workspace-panel-logicAnalyzer");

    expect(terminalTab.getAttribute("aria-selected")).toBe("true");
    expect(terminalTab.tabIndex).toBe(0);
    expect(logicTab.getAttribute("aria-selected")).toBe("false");
    expect(logicTab.tabIndex).toBe(-1);
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

  it("keeps the serial VIN callback mapped to the board vin switch", () => {
    const { host } = mountApp();
    const setVin = byTestId(host, "set-vin");

    click(setVin);

    expect(boardMocks.setSwitch).toHaveBeenCalledOnce();
    expect(boardMocks.setSwitch).toHaveBeenCalledWith("vin", "1.8v");
  });
});
