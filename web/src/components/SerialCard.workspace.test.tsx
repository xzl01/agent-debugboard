// @vitest-environment jsdom

import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { SerialCard, type SerialConnectionSummary } from "./SerialCard";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const serialMock = vi.hoisted(() => ({
  statuses: {
    uart0: {
      connected: true,
      connecting: false,
      automationActive: false,
      brokerWriteLocked: false,
      source: "webserial",
      portInfo: "CH347F · Web Serial",
      baud: 115200,
      lineEnding: "lf",
      rxBytes: 128,
      txBytes: 12,
    },
    uart1: {
      connected: false,
      connecting: false,
      automationActive: false,
      brokerWriteLocked: false,
      source: null,
      portInfo: "",
      baud: 921600,
      lineEnding: "crlf",
      rxBytes: 0,
      txBytes: 0,
    },
  } as Record<string, Record<string, unknown>>,
  connectWebSerial: { uart0: vi.fn(), uart1: vi.fn() },
  connectBridge: { uart0: vi.fn(), uart1: vi.fn() },
  disconnect: { uart0: vi.fn(async () => {}), uart1: vi.fn(async () => {}) },
}));

vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({
    t: (key: string) => ({
      "serial.title": "Target Serial Console",
      "serial.channels": "Serial channels",
      "serial.hostLogs.title": "Host UART archive",
      "serial.connectedCount": "{count}/2 connected",
      "serial.connected": "Connected",
      "serial.disconnected": "Disconnected",
      "serial.connecting": "Connecting",
      "serial.webSerial": "Web Serial",
      "serial.bridge": "Bridge",
      "serial.disconnect": "Disconnect",
      "serial.noConnection": "No connection yet.",
      "serial.connect": "Connect to the target UART.",
      "serial.none": "none",
      "serial.dualHint": "Both channels remain connected while switching tabs.",
      "serial.setupClose": "Close",
    }[key] ?? key),
  }),
}));

vi.mock("./HostSerialLogs", () => ({
  HostSerialLogs: () => <div data-testid="host-log-panel">Host logs</div>,
}));

vi.mock("./SerialTerminalPane", async () => {
  const React = await import("react");
  const SerialTerminalPane = React.forwardRef(function MockSerialTerminalPane(
    props: Record<string, unknown>,
    ref: React.ForwardedRef<Record<string, unknown>>
  ) {
    const channel = props.channel as "uart0" | "uart1";
    const status = serialMock.statuses[channel];
    React.useEffect(() => {
      (props.onStatus as (channel: string, status: Record<string, unknown>) => void)(channel, status);
    }, [channel, props.onStatus, status]);
    React.useImperativeHandle(ref, () => ({
      isConnected: () => Boolean(status.connected),
      isAutomationActive: () => Boolean(status.automationActive),
      connectWebSerial: serialMock.connectWebSerial[channel],
      connectBridge: serialMock.connectBridge[channel],
      disconnect: serialMock.disconnect[channel],
      clear: vi.fn(),
      write: vi.fn(async () => {}),
      setAutomationActive: vi.fn(),
      subscribe: vi.fn(() => () => {}),
    }), [channel, status]);
    return <div data-testid={`terminal-${channel}`}>{channel}</div>;
  });
  return {
    SerialTerminalPane,
    isSerialDisconnectBlocked: (status: { automationActive: boolean; connecting: boolean }) =>
      status.automationActive || status.connecting,
  };
});

type View = { host: HTMLDivElement; root: Root };
let view: View | null = null;

function mount(
  onConnectionChange?: (connections: SerialConnectionSummary) => void,
): View {
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  act(() => root.render(
    <SerialCard
      onConnectionChange={onConnectionChange}
    />,
  ));
  view = { host, root };
  return view;
}

function click(element: Element | null): void {
  if (!(element instanceof HTMLElement)) throw new TypeError("Expected clickable element");
  act(() => element.click());
}

async function flush(): Promise<void> {
  await act(async () => {
    await Promise.resolve();
    await Promise.resolve();
  });
}

beforeEach(() => {
  Object.defineProperty(window, "isSecureContext", { configurable: true, value: true });
  Object.defineProperty(navigator, "serial", {
    configurable: true,
    value: { getPorts: vi.fn(async () => []), requestPort: vi.fn() },
  });
  Object.assign(serialMock.statuses.uart0, {
    connected: true,
    connecting: false,
    automationActive: false,
    brokerWriteLocked: false,
    source: "webserial",
    portInfo: "CH347F · Web Serial",
  });
  Object.assign(serialMock.statuses.uart1, {
    connected: false,
    connecting: false,
    automationActive: false,
    brokerWriteLocked: false,
    source: null,
    portInfo: "",
  });
  vi.spyOn(window, "confirm").mockReturnValue(true);
});

afterEach(() => {
  if (view) act(() => view?.root.unmount());
  view?.host.remove();
  view = null;
  vi.restoreAllMocks();
  vi.clearAllMocks();
});

describe("SerialCard practical workspace", () => {
  it("reports the live channel summary to the workbench status bar", async () => {
    const onConnectionChange = vi.fn();
    mount(onConnectionChange);
    await flush();

    expect(onConnectionChange).toHaveBeenLastCalledWith({
      uart0: true,
      uart1: false,
      bridgeActive: false,
    });
  });

  it("reports whether any connected channel uses Host Bridge", async () => {
    Object.assign(serialMock.statuses.uart1, {
      connected: true,
      source: "bridge",
    });
    const onConnectionChange = vi.fn();
    mount(onConnectionChange);
    await flush();

    expect(onConnectionChange).toHaveBeenLastCalledWith({
      uart0: true,
      uart1: true,
      bridgeActive: true,
    });
  });

  it("keeps the terminal primary and moves connections and Host archives into one drawer", async () => {
    const { host } = mount();
    await flush();

    expect(host.querySelector('[data-testid="terminal-uart0"]')).not.toBeNull();
    expect(host.querySelector('[data-testid="host-log-panel"]')).toBeNull();

    click(host.querySelector('button[aria-label="Serial channels / Host UART archive"]'));
    expect(host.querySelector('[data-testid="serial-context-drawer"]')).not.toBeNull();
    expect(host.querySelector('[data-testid="serial-session-uart0"]')?.textContent).toContain("115,200 · LF");
    expect(host.querySelector('[data-testid="serial-session-uart1"]')?.textContent).toContain("921,600 · CRLF");

    const uart1 = host.querySelector('[data-testid="serial-session-uart1"]');
    const bridge = [...(uart1?.querySelectorAll("button") ?? [])].find((button) => button.textContent?.includes("Bridge"));
    click(bridge ?? null);
    expect(serialMock.connectBridge.uart1).toHaveBeenCalledOnce();

    const archiveTab = [...host.querySelectorAll('[role="tab"]')].find((tab) => tab.textContent === "Host UART archive");
    click(archiveTab ?? null);
    expect(host.querySelector('[data-testid="host-log-panel"]')).not.toBeNull();
  });

  it("allows a browser subscriber to disconnect when only the remote broker write claim is active", async () => {
    serialMock.statuses.uart0.brokerWriteLocked = true;
    const { host } = mount();
    await flush();

    click(host.querySelector('button[aria-label="Serial channels / Host UART archive"]'));
    const disconnect = host.querySelector('[data-testid="serial-session-uart0"] button');
    expect(disconnect).toBeInstanceOf(HTMLButtonElement);
    expect((disconnect as HTMLButtonElement).disabled).toBe(false);

    click(disconnect);
    await flush();
    expect(serialMock.disconnect.uart0).toHaveBeenCalledOnce();
  });
});
