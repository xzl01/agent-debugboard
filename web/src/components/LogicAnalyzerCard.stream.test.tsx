import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import { LogicAnalyzerCard } from "./LogicAnalyzerCard";
import { SigrokFrameType } from "@/lib/sigrokClient";

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
  afterEach(() => {
    vi.clearAllMocks();
    document.body.replaceChildren();
  });

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

  it("reports stream activity only after START resolves and clears it on unmount", async () => {
    const connection = deferred<void>();
    const configuration = deferred<{ actualRateKhz: number }>();
    const start = deferred<void>();
    const frame = deferred<null>();
    const onActivityChange = vi.fn();
    sigrokMocks.ensureConnected.mockReturnValue(connection.promise);
    sigrokMocks.configure.mockReturnValue(configuration.promise);
    sigrokMocks.start.mockReturnValue(start.promise);
    sigrokMocks.readCaptureFrame.mockReturnValue(frame.promise);
    const host = document.createElement("div");
    const root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard onActivityChange={onActivityChange} />));

    const button = Array.from(host.querySelectorAll("button")).find((candidate) =>
      candidate.textContent?.includes("logicAnalyzer.startStream")
    );
    await act(async () => {
      button?.click();
      await Promise.resolve();
    });
    expect(onActivityChange).not.toHaveBeenCalledWith(true);

    await act(async () => {
      connection.resolve();
      await Promise.resolve();
    });
    expect(onActivityChange).not.toHaveBeenCalledWith(true);

    await act(async () => {
      configuration.resolve({ actualRateKhz: 1000 });
      await Promise.resolve();
    });
    expect(onActivityChange).not.toHaveBeenCalledWith(true);

    await act(async () => {
      start.resolve();
      await Promise.resolve();
    });
    expect(onActivityChange).toHaveBeenLastCalledWith(true);

    act(() => root.unmount());
    expect(onActivityChange).toHaveBeenLastCalledWith(false);
  });

  it("reports bounded-capture activity only after START resolves and clears it on unmount", async () => {
    const connection = deferred<void>();
    const configuration = deferred<{ actualRateKhz: number }>();
    const start = deferred<void>();
    const frame = deferred<null>();
    const onActivityChange = vi.fn();
    sigrokMocks.ensureConnected.mockReturnValue(connection.promise);
    sigrokMocks.configure.mockReturnValue(configuration.promise);
    sigrokMocks.start.mockReturnValue(start.promise);
    sigrokMocks.readCaptureFrame.mockReturnValue(frame.promise);
    const host = document.createElement("div");
    const root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard onActivityChange={onActivityChange} />));

    const button = Array.from(host.querySelectorAll("button")).find((candidate) =>
      candidate.textContent?.includes("logicAnalyzer.arm")
    );
    await act(async () => {
      button?.click();
      await Promise.resolve();
    });
    expect(onActivityChange).not.toHaveBeenCalledWith(true);

    await act(async () => {
      connection.resolve();
      await Promise.resolve();
    });
    expect(onActivityChange).not.toHaveBeenCalledWith(true);

    await act(async () => {
      configuration.resolve({ actualRateKhz: 1000 });
      await Promise.resolve();
    });
    expect(onActivityChange).not.toHaveBeenCalledWith(true);

    await act(async () => {
      start.resolve();
      await Promise.resolve();
    });
    expect(onActivityChange).toHaveBeenLastCalledWith(true);

    act(() => root.unmount());
    expect(onActivityChange).toHaveBeenLastCalledWith(false);
  });

  it.each([
    ["stream", "logicAnalyzer.startStream"],
    ["bounded capture", "logicAnalyzer.arm"],
  ])("does not revive %s activity when START resolves after unmount", async (_mode, label) => {
    const start = deferred<void>();
    const onActivityChange = vi.fn();
    sigrokMocks.ensureConnected.mockResolvedValue(undefined);
    sigrokMocks.configure.mockResolvedValue({ actualRateKhz: 1000 });
    sigrokMocks.start.mockReturnValue(start.promise);
    sigrokMocks.stop.mockResolvedValue(undefined);
    const host = document.createElement("div");
    const root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard onActivityChange={onActivityChange} />));

    const button = Array.from(host.querySelectorAll("button")).find((candidate) =>
      candidate.textContent?.includes(label)
    );
    await act(async () => {
      button?.click();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(sigrokMocks.start).toHaveBeenCalledOnce();

    act(() => root.unmount());
    expect(onActivityChange).toHaveBeenLastCalledWith(false);
    await act(async () => {
      start.resolve();
      await Promise.resolve();
      await Promise.resolve();
    });

    expect(onActivityChange).not.toHaveBeenCalledWith(true);
    expect(onActivityChange).toHaveBeenLastCalledWith(false);
  });

  it("starts in capture mode and explains why decode is unavailable", () => {
    const host = document.createElement("div");
    const root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard />));

    const tabs = Array.from(host.querySelectorAll<HTMLButtonElement>('[role="tab"]'));
    expect(tabs).toHaveLength(2);
    expect(tabs[0]?.getAttribute("aria-selected")).toBe("true");
    expect(tabs[1]?.disabled).toBe(true);
    expect(tabs[1]?.title).toBe("logicAnalyzer.mode.decodeUnavailable");
    const capabilityDetails = Array.from(host.querySelectorAll("details")).find((item) =>
      item.textContent?.includes("logicAnalyzer.capabilityDetails")
    );
    expect(capabilityDetails).toBeDefined();
    expect(capabilityDetails?.open).toBe(false);
    act(() => root.unmount());
    expect(sigrokMocks.stop).not.toHaveBeenCalled();
    expect(sigrokMocks.close).toHaveBeenCalledTimes(1);
  });

  it("opens protocol decode after a bounded capture and can return without clearing it", async () => {
    sigrokMocks.ensureConnected.mockResolvedValue(undefined);
    sigrokMocks.configure.mockResolvedValue({ actualRateKhz: 1000 });
    sigrokMocks.start.mockResolvedValue(undefined);
    sigrokMocks.stop.mockResolvedValue(undefined);
    sigrokMocks.readCaptureFrame.mockResolvedValueOnce({
      type: SigrokFrameType.DATA,
      id: 1,
      meta: { sampleIndex: 0, sampleCount: 4, compression: 0, channelMask: 0x0008 },
      samples: new Uint8Array([0, 8, 0, 8]),
    });
    const host = document.createElement("div");
    document.body.append(host);
    const root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard />));

    const postSamplesLabel = Array.from(host.querySelectorAll("label")).find((label) =>
      label.textContent?.includes("logicAnalyzer.postSamples")
    );
    const postSamplesInput = postSamplesLabel?.querySelector("input");
    await act(async () => {
      if (postSamplesInput) {
        const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")?.set;
        setter?.call(postSamplesInput, "4");
        postSamplesInput.dispatchEvent(new Event("input", { bubbles: true }));
        postSamplesInput.dispatchEvent(new Event("change", { bubbles: true }));
      }
      await Promise.resolve();
    });

    const armButton = Array.from(host.querySelectorAll("button")).find((candidate) =>
      candidate.textContent?.includes("logicAnalyzer.arm")
    );
    await act(async () => {
      armButton?.click();
      await Promise.resolve();
      await Promise.resolve();
      await Promise.resolve();
    });

    const tabs = Array.from(host.querySelectorAll<HTMLButtonElement>('[role="tab"]'));
    expect(tabs[1]?.disabled).toBe(false);
    expect(tabs[1]?.getAttribute("aria-selected")).toBe("true");
    expect(host.textContent).toContain("logicAnalyzer.decoder.title");

    const newCaptureButton = Array.from(host.querySelectorAll("button")).find((candidate) =>
      candidate.textContent?.includes("logicAnalyzer.returnCapture")
    );
    act(() => newCaptureButton?.click());
    expect(tabs[0]?.getAttribute("aria-selected")).toBe("true");
    expect(tabs[1]?.disabled).toBe(false);
    act(() => root.unmount());
  });

  it("treats bounded capture cancellation as intentional and locks post-sample edits", async () => {
    const frame = deferred<null>();
    sigrokMocks.ensureConnected.mockResolvedValue(undefined);
    sigrokMocks.configure.mockResolvedValue({ actualRateKhz: 1000 });
    sigrokMocks.start.mockResolvedValue(undefined);
    sigrokMocks.stop.mockResolvedValue(undefined);
    sigrokMocks.readCaptureFrame.mockReturnValue(frame.promise);
    const host = document.createElement("div");
    document.body.append(host);
    const root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard />));

    const armButton = Array.from(host.querySelectorAll("button")).find((candidate) =>
      candidate.textContent?.includes("logicAnalyzer.arm")
    );
    await act(async () => {
      armButton?.click();
      await Promise.resolve();
      await Promise.resolve();
    });

    const postSamplesLabel = Array.from(host.querySelectorAll("label")).find((label) =>
      label.textContent?.includes("logicAnalyzer.postSamples")
    );
    expect(postSamplesLabel?.querySelector("input")?.disabled).toBe(true);
    const cancelButton = Array.from(host.querySelectorAll("button")).find((candidate) =>
      candidate.textContent?.includes("logicAnalyzer.cancel")
    );
    expect(cancelButton).toBeDefined();

    await act(async () => {
      cancelButton?.click();
      await Promise.resolve();
      frame.resolve(null);
      await Promise.resolve();
      await Promise.resolve();
    });

    expect(host.textContent).not.toContain("Timed out waiting for capture completion");
    expect(host.textContent).not.toContain("Trigger timeout");
    expect(sigrokMocks.stop).toHaveBeenCalled();
    act(() => root.unmount());
  });
});
