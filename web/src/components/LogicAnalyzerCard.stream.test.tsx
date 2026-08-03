import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import { LogicAnalyzerCard } from "./LogicAnalyzerCard";

const sigrokMocks = vi.hoisted(() => ({
  ensureConnected: vi.fn(),
  close: vi.fn(),
  configure: vi.fn(),
  start: vi.fn(),
  stop: vi.fn(),
  getServerCapabilities: vi.fn(() => ({
    hello: null,
    caps: null,
    serverFlags: 0,
    supportsConfigV2: false,
    supportsGenericPackedBurst: false,
  })),
  readCaptureFrame: vi.fn(),
}));

vi.mock("@/hooks/useSigrokScope", () => ({
  useSigrokScope: () => sigrokMocks,
}));

vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({ t: (key: string) => key }),
}));

vi.mock("./GpioPinoutSvg", () => ({
  GpioPinoutSvg: () => <div data-testid="gpio-pinout" />,
}));

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

function deferred<T>() {
  let resolve: (value: T) => void = () => {};
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
}

describe("LogicAnalyzerCard stream startup", () => {
  afterEach(() => vi.clearAllMocks());

  it("locks the stream action before awaiting the live-session connection", () => {
    const connection = deferred<void>();
    sigrokMocks.ensureConnected.mockReturnValue(connection.promise);
    sigrokMocks.stop.mockResolvedValue(undefined);
    const host = document.createElement("div");
    const root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard />));

    const button = Array.from(host.querySelectorAll("button")).find((candidate) =>
      candidate.textContent?.includes("logicAnalyzer.startStream")
    );
    expect(button).toBeDefined();

    act(() => {
      button?.dispatchEvent(new MouseEvent("click", { bubbles: true }));
      button?.dispatchEvent(new MouseEvent("click", { bubbles: true }));
    });

    expect(sigrokMocks.ensureConnected).toHaveBeenCalledTimes(1);
    expect((button as HTMLButtonElement).disabled).toBe(true);
    const armButton = Array.from(host.querySelectorAll("button")).find((candidate) =>
      candidate.textContent?.includes("logicAnalyzer.arm")
    );
    expect((armButton as HTMLButtonElement).disabled).toBe(true);
    expect(Array.from(host.querySelectorAll("select")).every((select) => select.disabled)).toBe(true);
    act(() => root.unmount());
  });
});
