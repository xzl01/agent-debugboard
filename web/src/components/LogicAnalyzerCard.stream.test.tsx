import { act, StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { LogicAnalyzerCard } from "./LogicAnalyzerCard";
import { SigrokEventCode, SigrokFrameType, type SigrokServerCapabilities } from "@/lib/sigrokClient";
import type { LogicDecoderResult } from "@/lib/logicDecoder";

const sigrokMocks = vi.hoisted(() => ({
  ensureConnected: vi.fn(),
  close: vi.fn(),
  configure: vi.fn(),
  start: vi.fn(),
  stop: vi.fn(),
  getServerCapabilities: vi.fn((): SigrokServerCapabilities => ({
    hello: null,
    caps: null,
    serverFlags: 0,
    supportsConfigV2: false,
    supportsGenericPackedBurst: false,
  })),
  readCaptureFrame: vi.fn(),
}));
const decoderMocks = vi.hoisted(() => ({ decode: vi.fn() }));
const waveformMocks = vi.hoisted(() => ({ path: vi.fn() }));

vi.mock("@/hooks/useSigrokScope", () => ({
  useSigrokScope: () => sigrokMocks,
}));
vi.mock("@/lib/logicDecoder", async (importOriginal) => ({
  ...await importOriginal<typeof import("@/lib/logicDecoder")>(),
  decodeLogicCapture: decoderMocks.decode,
}));
vi.mock("./logicAnalyzerWaveform", async (importOriginal) => {
  const original = await importOriginal<typeof import("./logicAnalyzerWaveform")>();
  waveformMocks.path.mockImplementation(original.stepWaveformPath);
  return { ...original, stepWaveformPath: waveformMocks.path };
});

vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({ t: (key: string) => key }),
}));

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

function deferred<T>() {
  let resolve: (value: T) => void = () => {};
  let reject: (reason: unknown) => void = () => {};
  const promise = new Promise<T>((done, fail) => { resolve = done; reject = fail; });
  return { promise, resolve, reject };
}

function inputFor(host: HTMLElement, key: string): HTMLInputElement | HTMLSelectElement {
  const input = Array.from(host.querySelectorAll("label")).find((label) =>
    label.textContent?.startsWith(key))?.querySelector("input, select");
  if (!(input instanceof HTMLInputElement || input instanceof HTMLSelectElement)) {
    throw new Error(`Missing input ${key}`);
  }
  return input;
}

function dataFrame(start = 0, samples = new Uint8Array([0, 1, 0, 1]), channelMask = 0x0008) {
  return {
    type: SigrokFrameType.DATA, id: 1,
    meta: { sampleIndex: start, sampleCount: samples.length, compression: 0, channelMask }, samples,
  };
}

function eventFrame(typeDetail: number, sampleIndex = 0) {
  return { type: SigrokFrameType.EVENT, id: 1, payload: { sessionId: 1, typeDetail, sampleIndex } };
}

const genericPackedBurstCaps: SigrokServerCapabilities = {
  hello: { protocolVersion: 1, serverFlags: 3, modeCount: 2, maxPayloadLen: 4096 },
  caps: null, serverFlags: 3, supportsConfigV2: true, supportsGenericPackedBurst: true,
};

function setupCapture() {
  sigrokMocks.ensureConnected.mockResolvedValue(undefined);
  sigrokMocks.configure.mockResolvedValue({ actualRateKhz: 500 });
  sigrokMocks.start.mockResolvedValue(undefined);
  sigrokMocks.stop.mockResolvedValue(undefined);
  sigrokMocks.readCaptureFrame.mockResolvedValueOnce(dataFrame());
  const host = document.createElement("div"); document.body.append(host);
  const root = createRoot(host);
  act(() => root.render(<LogicAnalyzerCard />));
  act(() => changeValue(inputFor(host, "logicAnalyzer.postSamples"), "4"));
  return { host, root };
}

function action(host: HTMLElement, key: string): HTMLButtonElement {
  const button = Array.from(host.querySelectorAll("button")).find((candidate) => candidate.textContent?.includes(key));
  if (!button) throw new Error(`Missing action ${key}`);
  return button;
}

function changeValue(element: HTMLInputElement | HTMLSelectElement, value: string) {
  const prototype = element instanceof HTMLInputElement ? HTMLInputElement.prototype : HTMLSelectElement.prototype;
  Object.getOwnPropertyDescriptor(prototype, "value")!.set!.call(element, value);
  element.dispatchEvent(new Event(element instanceof HTMLInputElement ? "input" : "change", { bubbles: true }));
}

function assertUnifiedLayout(host: HTMLElement) {
  const settings = host.querySelector('[data-testid="logic-analyzer-sampling-settings"]')!;
  const pins = host.querySelector('[data-testid="logic-analyzer-pin-select"]')!;
  const controls = host.querySelector('[data-testid="logic-analyzer-controls"]')!;
  const waveform = host.querySelector('[data-testid="logic-analyzer-waveform"]')!;
  const decoder = host.querySelector('[data-testid="logic-analyzer-decoder"]')!;
  expect(controls.children).toHaveLength(2);
  expect(settings.parentElement).toBe(controls);
  expect(pins.parentElement).toBe(controls);
  expect(waveform.parentElement).toBe(controls.parentElement);
  expect(controls.compareDocumentPosition(waveform) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
  expect(waveform.compareDocumentPosition(decoder) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
  expect(settings.querySelector('[data-testid="logic-analyzer-waveform-plot"]')).toBeNull();
  expect(decoder.querySelector("svg[role='img']")).toBeNull();
  expect(host.querySelector('[data-testid="logic-analyzer-mode-switch"]')).toBeNull();
  expect(host.querySelector('[role="tabpanel"]')).toBeNull();
}

describe("LogicAnalyzerCard stream startup", () => {
  beforeEach(() => {
    sigrokMocks.ensureConnected.mockReset().mockResolvedValue(undefined);
    sigrokMocks.configure.mockReset().mockResolvedValue({ actualRateKhz: 500 });
    sigrokMocks.start.mockReset().mockResolvedValue(undefined);
    sigrokMocks.stop.mockReset().mockResolvedValue(undefined);
    sigrokMocks.close.mockReset();
    sigrokMocks.getServerCapabilities.mockReset().mockReturnValue({
      hello: null, caps: null, serverFlags: 0, supportsConfigV2: false, supportsGenericPackedBurst: false,
    });
    sigrokMocks.readCaptureFrame.mockReset().mockReturnValue(deferred<null>().promise);
    decoderMocks.decode.mockReset().mockResolvedValue({
      schemaVersion: "radxa.logic-decoder.result.v1", diagnostics: [],
      annotations: [{ startSample: 1, endSample: 3, row: "uart", class: "data",
        shortText: "0x55", longText: "UART data 0x55", data: { value: 0x55 } }],
    });
  });
  afterEach(async () => {
    await act(async () => {});
    vi.clearAllMocks();
    vi.useRealTimers();
    document.body.replaceChildren();
  });

  it("starts the first stream after StrictMode effect cleanup/setup replay without cancelling START", async () => {
    sigrokMocks.ensureConnected.mockResolvedValue(undefined);
    sigrokMocks.configure.mockResolvedValue({ actualRateKhz: 500 });
    sigrokMocks.start.mockResolvedValue(undefined);
    sigrokMocks.stop.mockResolvedValue(undefined);
    sigrokMocks.readCaptureFrame.mockReturnValue(deferred<null>().promise);
    const onActivityChange = vi.fn();
    const host = document.createElement("div"); const root = createRoot(host);
    act(() => root.render(<StrictMode><LogicAnalyzerCard onActivityChange={onActivityChange} /></StrictMode>));
    // Real React development replay has already run an idle cleanup.
    expect(sigrokMocks.close).toHaveBeenCalledOnce();
    await act(async () => { action(host, "logicAnalyzer.startStream").click(); });
    expect(sigrokMocks.start).toHaveBeenCalledOnce();
    expect(sigrokMocks.stop).not.toHaveBeenCalled();
    expect(onActivityChange).toHaveBeenLastCalledWith(true);
    expect(action(host, "logicAnalyzer.stopStream")).toBeDefined();
    act(() => root.unmount());
  });

  const startupCases = (["stream", "bounded"] as const).flatMap((mode) =>
    (["ensureConnected", "configure", "start"] as const).flatMap((stage) =>
      (["cancel", "unmount"] as const).flatMap((boundary) =>
        (["resolve", "reject"] as const).map((completion) => ({ mode, stage, boundary, completion })))));

  it.each(startupCases)("ignores late $stage $completion after $boundary of $mode startup, including after a new stream starts", async ({ mode, stage, boundary, completion }) => {
    const waiting = deferred<unknown>();
    sigrokMocks[stage].mockReturnValueOnce(waiting.promise);
    const onActivityChange = vi.fn();
    const host = document.createElement("div"); document.body.append(host);
    let root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard onActivityChange={onActivityChange} />));
    await act(async () => { action(host, mode === "stream" ? "logicAnalyzer.startStream" : "logicAnalyzer.arm").click(); });
    expect(sigrokMocks[stage]).toHaveBeenCalledOnce();
    expect(onActivityChange).not.toHaveBeenCalledWith(true);
    if (boundary === "cancel") {
      await act(async () => { action(host, "logicAnalyzer.cancel").click(); });
    } else {
      await act(async () => { root.unmount(); });
      root = createRoot(host);
      act(() => root.render(<LogicAnalyzerCard onActivityChange={onActivityChange} />));
    }
    expect(sigrokMocks.stop).toHaveBeenCalledOnce();
    expect(sigrokMocks.close).toHaveBeenCalledOnce();
    await act(async () => { action(host, "logicAnalyzer.startStream").click(); });
    expect(onActivityChange).toHaveBeenLastCalledWith(true);
    const calls = onActivityChange.mock.calls.slice();
    const configureCount = sigrokMocks.configure.mock.calls.length;
    const startCount = sigrokMocks.start.mock.calls.length;
    await act(async () => {
      if (completion === "reject") waiting.reject(new Error("obsolete startup failed"));
      else waiting.resolve(stage === "configure" ? { actualRateKhz: 1000 } : undefined);
    });
    expect(sigrokMocks.configure).toHaveBeenCalledTimes(configureCount);
    expect(sigrokMocks.start).toHaveBeenCalledTimes(startCount);
    expect(sigrokMocks.stop).toHaveBeenCalledOnce();
    expect(sigrokMocks.close).toHaveBeenCalledOnce();
    expect(onActivityChange.mock.calls).toEqual(calls);
    expect(host.textContent).not.toContain("obsolete startup failed");
    expect(action(host, "logicAnalyzer.stopStream")).toBeDefined();
    expect(host.querySelector('[data-testid="logic-analyzer-waveform-rate"]')?.textContent)
      .toBe("logicAnalyzer.actualRate: 500.0 kHz");
    await act(async () => { root.unmount(); });
  });

  it("keeps the operation locked until STOP/close completes and releases it without waiting for the old read", async () => {
    const stop = deferred<void>(); const oldRead = deferred<unknown>();
    sigrokMocks.stop.mockReturnValueOnce(stop.promise);
    sigrokMocks.readCaptureFrame.mockReturnValueOnce(oldRead.promise);
    const onActivityChange = vi.fn();
    const host = document.createElement("div"); const root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard onActivityChange={onActivityChange} />));
    await act(async () => { action(host, "logicAnalyzer.startStream").click(); });
    await act(async () => { action(host, "logicAnalyzer.stopStream").click(); });
    expect(action(host, "logicAnalyzer.startStream").disabled).toBe(true);
    expect(action(host, "logicAnalyzer.arm").disabled).toBe(true);
    act(() => { action(host, "logicAnalyzer.startStream").click(); action(host, "logicAnalyzer.cancel").click(); });
    expect(sigrokMocks.ensureConnected).toHaveBeenCalledOnce();
    expect(sigrokMocks.stop).toHaveBeenCalledOnce();
    expect(sigrokMocks.close).not.toHaveBeenCalled();
    await act(async () => { stop.resolve(); });
    expect(sigrokMocks.close).toHaveBeenCalledOnce();
    expect(onActivityChange).toHaveBeenLastCalledWith(false);
    await act(async () => { action(host, "logicAnalyzer.startStream").click(); });
    const calls = onActivityChange.mock.calls.slice();
    await act(async () => { oldRead.reject(new Error("obsolete read failed")); });
    expect(sigrokMocks.stop).toHaveBeenCalledOnce();
    expect(sigrokMocks.close).toHaveBeenCalledOnce();
    expect(onActivityChange.mock.calls).toEqual(calls);
    expect(action(host, "logicAnalyzer.stopStream")).toBeDefined();
    expect(host.textContent).not.toContain("obsolete read failed");
    await act(async () => { root.unmount(); });
  });

  const readCases = (["stream", "bounded"] as const).flatMap((previous) =>
    (["stream", "bounded"] as const).flatMap((next) =>
      (["resolve", "reject"] as const).map((completion) => ({ previous, next, completion }))));

  it.each(readCases)("ignores an old $previous read $completion after stop/cancel and replacement by $next", async ({ previous, next, completion }) => {
    vi.useFakeTimers();
    const oldRead = deferred<unknown>();
    sigrokMocks.readCaptureFrame.mockReturnValueOnce(oldRead.promise)
      .mockResolvedValueOnce(dataFrame(0, new Uint8Array([1, 1, 1, 1])));
    const onActivityChange = vi.fn();
    const host = document.createElement("div"); document.body.append(host); const root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard onActivityChange={onActivityChange} />));
    act(() => changeValue(inputFor(host, "logicAnalyzer.postSamples"), "4"));
    await act(async () => { action(host, previous === "stream" ? "logicAnalyzer.startStream" : "logicAnalyzer.arm").click(); });
    await act(async () => { action(host, previous === "stream" ? "logicAnalyzer.stopStream" : "logicAnalyzer.cancel").click(); });
    await act(async () => { action(host, next === "stream" ? "logicAnalyzer.startStream" : "logicAnalyzer.arm").click(); });
    if (next === "stream") await act(async () => { await vi.advanceTimersByTimeAsync(150); });
    const path = host.querySelector('[data-logic-waveform-channel="13"]')?.getAttribute("d");
    expect(path).toBeDefined();
    const calls = onActivityChange.mock.calls.slice();
    const stopCount = sigrokMocks.stop.mock.calls.length;
    const closeCount = sigrokMocks.close.mock.calls.length;
    await act(async () => {
      if (completion === "reject") oldRead.reject(new Error("obsolete read failed"));
      else oldRead.resolve(dataFrame(0, new Uint8Array([0, 0, 0, 0])));
    });
    if (next === "stream") await act(async () => { await vi.advanceTimersByTimeAsync(150); });
    expect(host.querySelector('[data-logic-waveform-channel="13"]')?.getAttribute("d")).toBe(path);
    expect(sigrokMocks.stop).toHaveBeenCalledTimes(stopCount);
    expect(sigrokMocks.close).toHaveBeenCalledTimes(closeCount);
    expect(onActivityChange.mock.calls).toEqual(calls);
    expect(host.textContent).not.toContain("obsolete read failed");
    if (next === "bounded") {
      expect(action(host, "logicAnalyzer.reCapture")).toBeDefined();
      // A normally completed capture offers no direct Stream action. Clear is
      // the reachable boundary that releases both capture and decoder results.
      expect(host.textContent).not.toContain("logicAnalyzer.startStream");
      act(() => (host.querySelector('button[aria-label="logicAnalyzer.clear"]') as HTMLButtonElement).click());
      await act(async () => { action(host, "logicAnalyzer.startStream").click(); });
    }
    expect(action(host, "logicAnalyzer.stopStream")).toBeDefined();
    await act(async () => { root.unmount(); });
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
    expect(Array.from(host.querySelectorAll("select")).every((select) => select.matches(":disabled"))).toBe(true);
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

  it("shows one page with capture controls, a full-width waveform section and decoder preparation", () => {
    const host = document.createElement("div");
    const root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard />));

    assertUnifiedLayout(host);
    const tabs = Array.from(host.querySelectorAll<HTMLButtonElement>('[role="tab"]'));
    expect(tabs.map((tab) => tab.textContent)).toEqual(["UART", "I2C", "SPI"]);
    expect(tabs[0]?.getAttribute("aria-selected")).toBe("true");
    expect(action(host, "logicAnalyzer.decoder.decode").disabled).toBe(true);
    expect(host.textContent).toContain("logicAnalyzer.decoder.captureRequired");
    expect(host.querySelector('[data-testid="logic-analyzer-waveform-plot"]')).toBeNull();
    const capabilityDetails = Array.from(host.querySelectorAll("details")).find((item) =>
      item.textContent?.includes("logicAnalyzer.capabilityDetails")
    );
    expect(capabilityDetails).toBeDefined();
    expect(capabilityDetails?.open).toBe(false);
    act(() => root.unmount());
    expect(sigrokMocks.stop).not.toHaveBeenCalled();
    expect(sigrokMocks.close).toHaveBeenCalledTimes(1);
  });

  it("renders high-rate capability guidance without starting or configuring hardware", () => {
    const host = document.createElement("div"); document.body.append(host); const root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard />));
    act(() => changeValue(inputFor(host, "logicAnalyzer.sampleRate"), "100000000"));
    const notice = () => host.querySelector('[data-testid="logic-analyzer-high-rate-notice"]');
    expect(notice()?.textContent).toBe("logicAnalyzer.highRateConfigV2Required");
    sigrokMocks.getServerCapabilities.mockReturnValue({ ...genericPackedBurstCaps, supportsGenericPackedBurst: false });
    act(() => root.render(<LogicAnalyzerCard />));
    expect(notice()?.textContent).toContain("GENERIC_PACKED_BURST");
    sigrokMocks.getServerCapabilities.mockReturnValue(genericPackedBurstCaps);
    act(() => root.render(<LogicAnalyzerCard />));
    expect(notice()?.textContent).toBe("logicAnalyzer.streamHighRateAutoStopHint");
    expect(sigrokMocks.ensureConnected).not.toHaveBeenCalled();
    expect(sigrokMocks.configure).not.toHaveBeenCalled();
    expect(sigrokMocks.start).not.toHaveBeenCalled();
    expect(sigrokMocks.stop).not.toHaveBeenCalled();
    act(() => root.unmount());
  });

  it.each([100000000, 125000000].flatMap((rate) =>
    (["clear", "capture", "stream"] as const).map((replacement) => ({ rate, replacement }))))(
    "keeps the $rate Hz buffer-full snapshot before the first UI flush, navigates it, then replaces it with $replacement",
    async ({ rate, replacement }) => {
      vi.useFakeTimers();
      const startTime = Date.now();
      sigrokMocks.getServerCapabilities.mockReturnValue(genericPackedBurstCaps);
      sigrokMocks.configure.mockResolvedValueOnce({ actualRateKhz: rate / 1000 - 500 });
      sigrokMocks.readCaptureFrame
        .mockResolvedValueOnce(dataFrame(0, Uint8Array.from({ length: 50000 }, (_, index) => index % 2)))
        .mockResolvedValueOnce(dataFrame(50000, Uint8Array.from({ length: 50000 }, (_, index) => index % 2)))
        .mockResolvedValueOnce(eventFrame(SigrokEventCode.STOPPED, 100000));
      const onActivityChange = vi.fn();
      const host = document.createElement("div"); document.body.append(host); const root = createRoot(host);
      act(() => root.render(<LogicAnalyzerCard onActivityChange={onActivityChange} />));
      act(() => changeValue(inputFor(host, "logicAnalyzer.sampleRate"), String(rate)));
      expect(host.querySelector('[data-testid="logic-analyzer-high-rate-notice"]')?.textContent)
        .toBe("logicAnalyzer.streamHighRateAutoStopHint");
      await act(async () => { action(host, "logicAnalyzer.startStream").click(); });
      expect(Date.now()).toBe(startTime); // DATA + STOPPED occur before any 150 ms flush.
      expect(sigrokMocks.configure.mock.calls[0][0]).toMatchObject({ samplerateKhz: rate / 1000, preSamples: 0, postSamples: 0 });
      expect(sigrokMocks.readCaptureFrame).toHaveBeenCalledTimes(3);
      expect(sigrokMocks.stop).toHaveBeenCalledOnce();
      expect(sigrokMocks.close).toHaveBeenCalledOnce();
      expect(onActivityChange).toHaveBeenLastCalledWith(false);
      assertUnifiedLayout(host);
      const plot = () => host.querySelector<SVGElement>('[data-testid="logic-analyzer-waveform-plot"]')!;
      const bounds = () => [Number(plot().dataset.sampleStart), Number(plot().dataset.sampleEnd)];
      expect(plot()?.getAttribute("aria-label")).toBe("logicAnalyzer.streamSnapshot");
      expect(bounds()).toEqual([95904, 100000]);
      expect(plot().querySelector('[data-logic-waveform-channel="13"]')).not.toBeNull();
      expect(plot().querySelector('[data-logic-trigger]')).toBeNull();
      expect(host.querySelector('[data-testid="logic-analyzer-waveform"]')?.textContent).toContain("100.0K");
      expect(host.querySelector('[data-testid="logic-analyzer-waveform-rate"]')?.textContent)
        .toBe(`logicAnalyzer.actualRate: ${(rate / 1e6 - 0.5).toFixed(1)} MHz`);
      expect(host.querySelector('[data-testid="logic-analyzer-stream-requested-rate"]')?.textContent)
        .toBe(`logicAnalyzer.requestedRate: ${(rate / 1e6).toFixed(1)} MHz`);
      expect(host.querySelector('[data-testid="logic-analyzer-stream-notice"]')?.textContent).toBe("logicAnalyzer.streamAutoStopped");
      expect(host.textContent).toContain("logicAnalyzer.streamSnapshotReadOnly");
      expect(host.textContent).toContain("logicAnalyzer.decoder.streamStoppedHint");
      expect(host.textContent).not.toContain("logicAnalyzer.decoder.liveHint");
      expect(host.textContent).not.toContain("logicAnalyzer.captured");
      expect(host.textContent).not.toContain("CSV");
      expect(host.textContent).not.toContain("PulseView (.sr)");
      expect(action(host, "logicAnalyzer.decoder.decode").disabled).toBe(true);
      expect(plot().querySelector('[data-logic-annotation]')).toBeNull();

      const follow = host.querySelector<HTMLInputElement>('input[type="checkbox"]')!;
      act(() => follow.click());
      const slider = host.querySelector<HTMLInputElement>('input[type="range"]')!;
      act(() => changeValue(slider, "0"));
      expect(bounds()).toEqual([0, 4096]);
      act(() => changeValue(host.querySelector<HTMLSelectElement>('[aria-label="logicAnalyzer.streamWindow"]')!, "1024"));
      expect(bounds()).toEqual([3072, 4096]);
      act(() => follow.click());
      expect(bounds()).toEqual([98976, 100000]);

      // Editing future settings must not relabel the read-only snapshot or
      // reconcile its decoder pins to a different, uncaptured pin selection.
      const path = plot().querySelector('[data-logic-waveform-channel="13"]')?.getAttribute("d");
      act(() => {
        changeValue(inputFor(host, "logicAnalyzer.sampleRate"), "1000000");
        changeValue(inputFor(host, "logicAnalyzer.postSamples"), "4");
        for (const pin of [13, 12]) host.querySelector(`[data-logic-chip="${pin}"]`)!
          .dispatchEvent(new MouseEvent("contextmenu", { bubbles: true }));
      });
      expect(plot().querySelector('[data-logic-waveform-channel="13"]')?.getAttribute("d")).toBe(path);
      expect(host.querySelector('[data-testid="logic-analyzer-stream-requested-rate"]')?.textContent)
        .toBe(`logicAnalyzer.requestedRate: ${(rate / 1e6).toFixed(1)} MHz`);
      expect(inputFor(host, "RX").value).toBe("13");
      expect(sigrokMocks.configure).toHaveBeenCalledOnce();
      expect(sigrokMocks.start).toHaveBeenCalledOnce();
      expect(sigrokMocks.stop).toHaveBeenCalledOnce();
      expect(host.querySelector('[data-testid="logic-analyzer-stream-notice"]')?.textContent).toBe("logicAnalyzer.streamAutoStopped");

      if (replacement === "clear") {
        act(() => host.querySelector<HTMLButtonElement>('button[aria-label="logicAnalyzer.clear"]')!.click());
        expect(plot()).toBeNull();
        expect(host.querySelector('[data-testid="logic-analyzer-waveform-rate"]')).toBeNull();
        expect(host.textContent).toContain("logicAnalyzer.decoder.captureRequired");
        expect(sigrokMocks.configure).toHaveBeenCalledOnce();
        expect(sigrokMocks.stop).toHaveBeenCalledOnce();
      } else {
        const connection = deferred<void>();
        sigrokMocks.ensureConnected.mockReturnValueOnce(connection.promise);
        sigrokMocks.configure.mockResolvedValueOnce({ actualRateKhz: 500 });
        sigrokMocks.readCaptureFrame.mockResolvedValueOnce(dataFrame(0, new Uint8Array([1, 0, 1, 0]), 0x0004));
        await act(async () => { action(host, replacement === "capture" ? "logicAnalyzer.arm" : "logicAnalyzer.startStream").click(); });
        expect(plot()).toBeNull(); // Released at the new operation, not its first data frame.
        expect(host.querySelector('[data-testid="logic-analyzer-stream-notice"]')).toBeNull();
        await act(async () => { connection.resolve(); });
        if (replacement === "stream") await act(async () => { await vi.advanceTimersByTimeAsync(150); });
        expect(Array.from(plot().querySelectorAll('[data-logic-waveform-channel]')).map((channel) =>
          channel.getAttribute("data-logic-waveform-channel"))).toEqual(["12"]);
        expect(host.querySelector('[data-testid="logic-analyzer-waveform-rate"]')?.textContent)
          .toBe("logicAnalyzer.actualRate: 500.0 kHz");
        expect(host.textContent).not.toContain("logicAnalyzer.decoder.streamStoppedHint");
      }
      expect(host.querySelector('[data-stream-stop-reason]')).toBeNull();
      expect(host.querySelector('[data-testid="logic-analyzer-stream-notice"]')).toBeNull();
      expect(host.querySelector('[data-testid="logic-analyzer-stream-requested-rate"]')).toBeNull();
      act(() => root.unmount());
    }
  );

  it.each([
    [SigrokEventCode.STOPPED, "logicAnalyzer.streamUnexpectedStop"],
    [SigrokEventCode.OVERRUN, "Capture overrun"],
    [SigrokEventCode.ERROR, "Capture failed"],
  ])("marks retained partial samples incomplete after event %s rather than presenting a complete capture", async (event, message) => {
    vi.useFakeTimers();
    sigrokMocks.readCaptureFrame.mockResolvedValueOnce(dataFrame()).mockResolvedValueOnce(eventFrame(event as number, 4));
    const host = document.createElement("div"); document.body.append(host); const root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard />));
    await act(async () => { action(host, "logicAnalyzer.startStream").click(); });
    expect(host.querySelector('[data-stream-stop-reason="incomplete"]')).not.toBeNull();
    expect(host.querySelector('[data-testid="logic-analyzer-waveform-plot"]')).not.toBeNull();
    expect(host.querySelector('[data-testid="logic-analyzer-stream-notice"]')?.textContent).toBe("logicAnalyzer.streamIncomplete");
    expect(host.textContent).toContain(message);
    expect(host.textContent).toContain("logicAnalyzer.streamSnapshotIncomplete");
    expect(host.textContent).not.toContain("logicAnalyzer.streamAutoStopped");
    expect(host.textContent).not.toContain("logicAnalyzer.decoder.liveHint");
    expect(action(host, "logicAnalyzer.decoder.decode").disabled).toBe(true);
    expect(host.textContent).not.toContain("CSV");
    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="logicAnalyzer.clear"]')!.click());
    expect(host.textContent).not.toContain(message);
    expect(host.querySelector('[data-testid="logic-analyzer-stream-notice"]')).toBeNull();
    act(() => root.unmount());
  });

  it("keeps bounded capture, decode overlays, settings and exports on the same page", async () => {
    sigrokMocks.ensureConnected.mockResolvedValue(undefined);
    sigrokMocks.configure.mockResolvedValue({ actualRateKhz: 1000 });
    sigrokMocks.start.mockResolvedValue(undefined);
    sigrokMocks.stop.mockResolvedValue(undefined);
    sigrokMocks.readCaptureFrame.mockResolvedValueOnce({
      type: SigrokFrameType.EVENT, id: 1,
      payload: { sessionId: 1, typeDetail: SigrokEventCode.TRIGGERED, sampleIndex: 1 },
    }).mockResolvedValueOnce({
      type: SigrokFrameType.DATA,
      id: 1,
      meta: { sampleIndex: 0, sampleCount: 4, compression: 0, channelMask: 0x0008 },
      samples: new Uint8Array([0, 1, 0, 1]),
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
      const trigger = Array.from(host.querySelectorAll("label")).find((label) =>
        label.textContent?.startsWith("logicAnalyzer.triggerType"))!.querySelector("select")!;
      changeValue(trigger, "rising");
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

    assertUnifiedLayout(host);
    expect(host.textContent).toContain("logicAnalyzer.decoder.title");
    const plot = host.querySelector('[data-testid="logic-analyzer-waveform-plot"]')!;
    expect(plot).not.toBeNull();
    expect(host.querySelectorAll('[data-testid="logic-analyzer-waveform-plot"]')).toHaveLength(1);
    expect(Number(plot.querySelector('[data-logic-trigger]')?.getAttribute("x1"))).toBe(233);
    expect(action(host, "CSV")).toBeDefined();
    expect(action(host, "PulseView (.sr)")).toBeDefined();
    expect(action(host, "logicAnalyzer.reCapture")).toBeDefined();
    await act(async () => { action(host, "logicAnalyzer.decoder.decode").click(); await Promise.resolve(); });
    expect(decoderMocks.decode).toHaveBeenCalledTimes(1);
    expect(decoderMocks.decode.mock.calls[0][0].samples.words).toEqual([0, 1, 0, 1]);
    const annotation = plot.querySelector('[data-logic-annotation] rect')!;
    expect(Number(annotation.getAttribute("x"))).toBe(233);
    expect(Number(annotation.getAttribute("width"))).toBe(370);
    expect(plot.querySelector('[data-logic-waveform-channel="13"]')?.getAttribute("d")).toContain(" L 233 ");
    expect(plot.querySelector('[data-logic-waveform-channel="13"]')?.getAttribute("d")).toContain(" L 603 ");
    expect(plot.querySelector('[data-logic-annotation] title')?.textContent).toBe("UART data 0x55");
    expect(host.querySelectorAll('[data-testid="logic-analyzer-waveform-plot"]')).toHaveLength(1);
    expect(host.querySelector("table")?.textContent).toContain("0x55");
    const settings = host.querySelector('[data-testid="logic-analyzer-sampling-settings"]')!;
    expect(settings.querySelector<HTMLSelectElement>("select")?.disabled).toBe(false);

    // Changing decoder settings marks only the annotations stale, not the capture.
    act(() => {
      const baud = host.querySelector<HTMLInputElement>('[data-testid="logic-analyzer-decoder"] input[type="number"]')!;
      changeValue(baud, "57600");
    });
    expect(host.textContent).toContain("logicAnalyzer.decoder.staleWarning");
    expect(plot.querySelector("[data-logic-annotation]")).toBeNull();
    expect(plot.querySelector('[data-logic-waveform-channel="13"]')).not.toBeNull();
    await act(async () => { action(host, "logicAnalyzer.decoder.decode").click(); await Promise.resolve(); });
    expect(decoderMocks.decode).toHaveBeenCalledTimes(2);
    expect(plot.querySelector("[data-logic-annotation]")).not.toBeNull();
    act(() => (host.querySelector('button[aria-label="logicAnalyzer.clear"]') as HTMLButtonElement).click());
    assertUnifiedLayout(host);
    expect(host.querySelector('[data-testid="logic-analyzer-waveform-plot"]')).toBeNull();
    expect(action(host, "logicAnalyzer.decoder.decode").disabled).toBe(true);
    expect(host.querySelector("table")).toBeNull();
    expect(host.textContent).not.toContain("logicAnalyzer.decoder.staleWarning");
    act(() => root.unmount());
  });

  it("uses the same full-width plot for live annotations and keeps their original time coordinates as samples arrive", async () => {
    vi.useFakeTimers();
    const next = deferred<unknown>(); const pending = deferred<null>();
    const dataFrame = (start: number, samples: Uint8Array) => ({
      type: SigrokFrameType.DATA, id: 1,
      meta: { sampleIndex: start, sampleCount: samples.length, compression: 0, channelMask: 0x0008 }, samples,
    });
    sigrokMocks.ensureConnected.mockResolvedValue(undefined);
    sigrokMocks.configure.mockResolvedValue({ actualRateKhz: 500 });
    sigrokMocks.start.mockResolvedValue(undefined);
    sigrokMocks.stop.mockResolvedValue(undefined);
    sigrokMocks.readCaptureFrame.mockResolvedValueOnce(dataFrame(0,
      Uint8Array.from({ length: 1024 }, (_, index) => index >= 600 && index < 700 ? 1 : 0)))
      .mockReturnValueOnce(next.promise).mockReturnValue(pending.promise);
    decoderMocks.decode.mockResolvedValue({ schemaVersion: "radxa.logic-decoder.result.v1", diagnostics: [],
      annotations: [{ startSample: 600, endSample: 700, row: "uart", class: "data", shortText: "0x55", longText: "UART data 0x55", data: {} }] });
    const host = document.createElement("div"); document.body.append(host); const root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard />));
    await act(async () => { action(host, "logicAnalyzer.startStream").click(); await Promise.resolve(); });
    act(() => changeValue(host.querySelector<HTMLSelectElement>('[aria-label="logicAnalyzer.streamWindow"]')!, "1024"));
    await act(async () => { await vi.advanceTimersByTimeAsync(150); });
    await act(async () => { await vi.advanceTimersByTimeAsync(450); });
    assertUnifiedLayout(host);
    const plot = host.querySelector('[data-testid="logic-analyzer-waveform-plot"]')!;
    expect(plot.getAttribute("aria-label")).toBe("logicAnalyzer.streamWaveform");
    expect(host.querySelector('[data-testid="logic-analyzer-waveform-rate"]')?.textContent)
      .toBe("logicAnalyzer.actualRate: 500.0 kHz");
    expect(host.querySelector('[data-testid="logic-analyzer-config-preview"]')?.textContent)
      .toContain("logicAnalyzer.requestedRate: 1.0 MHz");
    expect(host.textContent?.match(/logicAnalyzer\.actualRate/g)).toHaveLength(1);
    expect(host.querySelectorAll('[data-testid="logic-analyzer-waveform-plot"]')).toHaveLength(1);
    expect(plot.querySelector("[data-logic-trigger]")).toBeNull();
    const rect = () => plot.querySelector('[data-logic-annotation] rect')!;
    expect(Number(rect().getAttribute("x"))).toBeCloseTo(48 + 600 / 1024 * 740);
    expect(Number(rect().getAttribute("width"))).toBeCloseTo(100 / 1024 * 740);
    expect(host.textContent).toContain("logicAnalyzer.decoder.liveHint");
    expect(action(host, "logicAnalyzer.decoder.decode").disabled).toBe(true);
    await act(async () => { next.resolve(dataFrame(1024, new Uint8Array(512))); await Promise.resolve(); });
    await act(async () => { await vi.advanceTimersByTimeAsync(150); });
    // No decoder rerun yet. The previous annotation is anchored to sample 600,
    // not spuriously shifted to sample 1112 by the new follow-window origin.
    expect(decoderMocks.decode).toHaveBeenCalledTimes(1);
    expect(decoderMocks.decode.mock.calls[0][0].samples.sampleRateHz).toBe(500000);
    expect(Number(rect().getAttribute("x"))).toBeCloseTo(48 + 88 / 1024 * 740);
    expect(plot.querySelector('[data-logic-waveform-channel="13"]')?.getAttribute("d"))
      .toContain(` L ${48 + 88 / 1024 * 740} `);
    await act(async () => { action(host, "logicAnalyzer.stopStream").click(); pending.resolve(null); await Promise.resolve(); });
    expect(sigrokMocks.stop).toHaveBeenCalled();
    expect(host.textContent).not.toContain("Timed out waiting for stream data");
    assertUnifiedLayout(host);
    expect(host.querySelector('[data-testid="logic-analyzer-waveform-plot"]')).not.toBeNull();
    expect(host.querySelector('[data-testid="logic-analyzer-stream-notice"]')?.textContent).toBe("logicAnalyzer.streamManuallyStopped");
    expect(host.querySelector('[data-stream-stop-reason="manual"]')).not.toBeNull();
    expect(plot.querySelector('[data-logic-annotation]')).toBeNull();
    expect(host.textContent).toContain("logicAnalyzer.decoder.stopped");
    expect(host.textContent).not.toContain("logicAnalyzer.decoder.liveHint");
    await act(async () => { await vi.advanceTimersByTimeAsync(1200); });
    expect(decoderMocks.decode).toHaveBeenCalledTimes(1);
    act(() => root.unmount());
  });

  it("freezes the absolute stream window, allows history navigation, and warns when the ring evicts it", async () => {
    vi.useFakeTimers();
    const append = deferred<unknown>(); const fill = deferred<unknown>();
    const ringFrames = Array.from({ length: 20 }, (_, index) => dataFrame(12292 + index * 50000, new Uint8Array(50000)));
    sigrokMocks.readCaptureFrame.mockResolvedValueOnce(dataFrame(0, new Uint8Array(8192)))
      .mockReturnValueOnce(append.promise).mockReturnValueOnce(fill.promise);
    for (const frame of ringFrames) sigrokMocks.readCaptureFrame.mockResolvedValueOnce(frame);
    const host = document.createElement("div"); document.body.append(host); const root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard />));
    await act(async () => { action(host, "logicAnalyzer.startStream").click(); });
    await act(async () => { await vi.advanceTimersByTimeAsync(150); });
    const plot = () => host.querySelector<SVGElement>('[data-testid="logic-analyzer-waveform-plot"]')!;
    const bounds = () => [Number(plot().dataset.sampleStart), Number(plot().dataset.sampleEnd)];
    const follow = host.querySelector<HTMLInputElement>('input[type="checkbox"]')!;
    expect(bounds()).toEqual([4096, 8192]);
    act(() => follow.click());
    expect(follow.checked).toBe(false);
    await act(async () => { append.resolve(dataFrame(8192, new Uint8Array(4096))); });
    await act(async () => { await vi.advanceTimersByTimeAsync(150); });
    expect(bounds()).toEqual([4096, 8192]);
    const slider = host.querySelector<HTMLInputElement>('input[type="range"]')!;
    act(() => changeValue(slider, "0"));
    expect(bounds()).toEqual([0, 4096]);
    await act(async () => { fill.resolve(dataFrame(12288, new Uint8Array(4))); });
    await act(async () => { await vi.advanceTimersByTimeAsync(150); });
    const retainedFirst = 8192 + 4096 + 4 + 1000000 - 1000000;
    expect(bounds()).toEqual([retainedFirst, retainedFirst + 4096]);
    expect(host.querySelector('[data-testid="logic-analyzer-history-evicted"]')?.textContent)
      .toBe("logicAnalyzer.streamHistoryEvicted");
    act(() => follow.click());
    expect(bounds()).toEqual([1012292 - 4096, 1012292]);
    expect(host.querySelector('[data-testid="logic-analyzer-history-evicted"]')).toBeNull();
    await act(async () => { action(host, "logicAnalyzer.stopStream").click(); });
    expect(bounds()).toEqual([1012292 - 4096, 1012292]);
    expect(host.textContent).toContain("logicAnalyzer.streamRetainedSamples");
    act(() => follow.click());
    const finalSlider = host.querySelector<HTMLInputElement>('input[type="range"]')!;
    expect(Number(finalSlider.max)).toBe(1000000 - 4096);
    act(() => changeValue(finalSlider, "0"));
    expect(bounds()).toEqual([12292, 12292 + 4096]);
    act(() => root.unmount());
  });

  it.each(["alternating", "single pulse"])("renders %s stream activity instead of a false constant level", async (signal) => {
    vi.useFakeTimers();
    const samples = Uint8Array.from({ length: 4096 }, (_, index) => signal === "alternating" ? index % 2 : Number(index === 7));
    sigrokMocks.readCaptureFrame.mockResolvedValueOnce(dataFrame(0, samples));
    const host = document.createElement("div"); document.body.append(host); const root = createRoot(host);
    act(() => root.render(<LogicAnalyzerCard />));
    await act(async () => { action(host, "logicAnalyzer.startStream").click(); });
    await act(async () => { await vi.advanceTimersByTimeAsync(150); });
    const path = host.querySelector('[data-logic-waveform-channel="13"]')!.getAttribute("d")!;
    expect(path).toMatch(/(?:M|L) [\d.]+ 20\b/);
    expect(path).toMatch(/(?:M|L) [\d.]+ 40\b/);
    expect(path).toContain(" L 788 ");
    act(() => root.unmount());
  });

  it("reuses five 100K-sample paths for pending settings, but updates layout and replacement capture channels", async () => {
    const { host, root } = setupCapture();
    sigrokMocks.readCaptureFrame.mockReset()
      .mockResolvedValueOnce(dataFrame(0, Uint8Array.from({ length: 50000 }, (_, index) => index % 32), 0x007c))
      .mockResolvedValueOnce(dataFrame(50000, Uint8Array.from({ length: 50000 }, (_, index) => index % 32), 0x007c));
    act(() => {
      for (const pin of [12, 14, 15, 16]) {
        host.querySelector(`[data-logic-chip="${pin}"]`)!.dispatchEvent(new MouseEvent("contextmenu", { bubbles: true }));
      }
      changeValue(inputFor(host, "logicAnalyzer.postSamples"), "65535");
    });
    await act(async () => { action(host, "logicAnalyzer.arm").click(); });
    expect(waveformMocks.path).toHaveBeenCalledTimes(5);
    expect(waveformMocks.path.mock.calls[0][0]).toHaveLength(100001);
    const path = host.querySelector('[data-logic-waveform-channel="13"]')!.getAttribute("d");
    const rate = host.querySelector('[data-testid="logic-analyzer-waveform-rate"]')!;
    expect(rate.textContent).toBe("logicAnalyzer.actualRate: 500.0 kHz");
    act(() => {
      for (const value of ["500000", "1000000", "2000000"]) changeValue(inputFor(host, "logicAnalyzer.sampleRate"), value);
      changeValue(inputFor(host, "logicAnalyzer.postSamples"), "4");
      for (const pin of [14, 15, 16]) {
        host.querySelector(`[data-logic-chip="${pin}"]`)!.dispatchEvent(new MouseEvent("contextmenu", { bubbles: true }));
      }
      host.querySelector('[data-logic-chip="13"]')!.dispatchEvent(new MouseEvent("mouseover", { bubbles: true }));
    });
    expect(waveformMocks.path).toHaveBeenCalledTimes(5);
    expect(host.querySelector('[data-logic-waveform-channel="13"]')!.getAttribute("d")).toBe(path);
    expect(rate.textContent).toBe("logicAnalyzer.actualRate: 500.0 kHz");
    expect(host.querySelector('[data-testid="logic-analyzer-config-preview"]')?.textContent)
      .toContain("logicAnalyzer.requestedRate: 2.0 MHz");
    expect(host.textContent?.match(/logicAnalyzer\.actualRate/g)).toHaveLength(1);

    // Annotation lanes change the real channel offset, so geometry must update.
    await act(async () => { action(host, "logicAnalyzer.decoder.decode").click(); });
    expect(waveformMocks.path).toHaveBeenCalledTimes(10);
    expect(waveformMocks.path.mock.calls[5][1]).toBe(44);
    expect(host.querySelector('[data-logic-waveform-channel="13"]')!.getAttribute("d")).not.toBe(path);
    // A new capture adopts the pending pins and data, not cached old geometry.
    sigrokMocks.configure.mockResolvedValueOnce({ actualRateKhz: 1000 });
    sigrokMocks.readCaptureFrame.mockResolvedValueOnce(dataFrame(0, new Uint8Array([3, 0, 3, 0]), 0x000c));
    await act(async () => { action(host, "logicAnalyzer.reCapture").click(); });
    expect(waveformMocks.path).toHaveBeenCalledTimes(12);
    expect(waveformMocks.path.mock.calls[10][0]).toHaveLength(5);
    expect(Array.from(host.querySelectorAll('[data-logic-waveform-channel]')).map((channel) => channel.getAttribute("data-logic-waveform-channel")))
      .toEqual(["12", "13"]);
    expect(host.querySelector('[data-testid="logic-analyzer-waveform-rate"]')?.textContent)
      .toBe("logicAnalyzer.actualRate: 1.0 MHz");
    act(() => root.unmount());
  });

  it.each(["resolve", "reject"] as const)("cancels loading on protocol changes without letting an old %s unlock a newer identical decode", async (completion) => {
    const { host, root } = setupCapture();
    const oldRun = deferred<LogicDecoderResult>(); const newRun = deferred<LogicDecoderResult>();
    const result = await decoderMocks.decode(); decoderMocks.decode.mockClear();
    decoderMocks.decode.mockReturnValueOnce(oldRun.promise).mockReturnValueOnce(newRun.promise);
    await act(async () => { action(host, "logicAnalyzer.arm").click(); });
    act(() => action(host, "logicAnalyzer.decoder.decode").click());
    expect(action(host, "logicAnalyzer.decoder.decoding").disabled).toBe(true);
    act(() => action(host, "I2C").click());
    expect(host.textContent).not.toContain("logicAnalyzer.decoder.decoding");
    expect(host.textContent).toContain("logicAnalyzer.decoder.error.duplicateChannel");
    act(() => action(host, "UART").click());
    expect(action(host, "logicAnalyzer.decoder.decode").disabled).toBe(false);
    act(() => action(host, "logicAnalyzer.decoder.decode").click());
    expect(decoderMocks.decode).toHaveBeenCalledTimes(2);
    await act(async () => {
      if (completion === "resolve") oldRun.resolve(result);
      else oldRun.reject(new Error("obsolete decode failed"));
    });
    expect(action(host, "logicAnalyzer.decoder.decoding").disabled).toBe(true);
    expect(host.textContent).not.toContain("obsolete decode failed");
    expect(host.querySelector('[data-logic-annotation]')).toBeNull();
    await act(async () => { newRun.resolve(result); });
    expect(action(host, "logicAnalyzer.decoder.decode").disabled).toBe(false);
    expect(host.querySelector('[data-logic-annotation]')).not.toBeNull();
    act(() => root.unmount());
  });

  it("shows invalid single-channel live I2C configuration and resumes after switching to UART", async () => {
    vi.useFakeTimers();
    const { host, root } = setupCapture(); const pending = deferred<null>();
    sigrokMocks.readCaptureFrame.mockReturnValue(pending.promise);
    act(() => action(host, "I2C").click());
    await act(async () => { action(host, "logicAnalyzer.startStream").click(); });
    expect(host.querySelector('[data-testid="logic-analyzer-live-decode-error"]')?.textContent)
      .toBe("logicAnalyzer.decoder.error.duplicateChannel");
    expect(host.querySelector('[data-testid="logic-analyzer-decoder"]')?.textContent)
      .toContain("logicAnalyzer.decoder.status.error");
    expect(host.textContent).not.toContain("logicAnalyzer.decoder.liveHint");
    await act(async () => { await vi.advanceTimersByTimeAsync(600); });
    expect(decoderMocks.decode).not.toHaveBeenCalled();
    await act(async () => { action(host, "UART").click(); });
    expect(host.querySelector('[data-testid="logic-analyzer-live-decode-error"]')).toBeNull();
    expect(decoderMocks.decode).toHaveBeenCalledOnce();
    await act(async () => { await vi.advanceTimersByTimeAsync(600); });
    expect(decoderMocks.decode).toHaveBeenCalledTimes(2);
    expect(host.querySelector('[data-logic-annotation]')).not.toBeNull();
    expect(host.textContent).toContain("logicAnalyzer.decoder.liveHint");
    await act(async () => { action(host, "logicAnalyzer.stopStream").click(); pending.resolve(null); });
    act(() => root.unmount());
  });

  it("reports live decoder failures, clears them after edits, and ignores obsolete protocol and stream failures", async () => {
    vi.useFakeTimers();
    const { host, root } = setupCapture(); const frame = deferred<null>();
    const oldProtocol = deferred<LogicDecoderResult>(); const oldStream = deferred<LogicDecoderResult>();
    decoderMocks.decode.mockReturnValueOnce(oldProtocol.promise).mockRejectedValueOnce(new Error("current decoder failed"))
      .mockReturnValueOnce(oldStream.promise);
    sigrokMocks.readCaptureFrame.mockReturnValue(frame.promise);
    await act(async () => { action(host, "logicAnalyzer.startStream").click(); });
    await act(async () => { await vi.advanceTimersByTimeAsync(150); });
    await act(async () => { await vi.advanceTimersByTimeAsync(450); });
    expect(decoderMocks.decode).toHaveBeenCalledOnce();
    await act(async () => { changeValue(inputFor(host, "logicAnalyzer.decoder.baud"), "57600"); });
    expect(decoderMocks.decode).toHaveBeenCalledTimes(2);
    expect(host.querySelector('[data-testid="logic-analyzer-live-decode-error"]')?.textContent).toBe("current decoder failed");
    await act(async () => { oldProtocol.reject(new Error("obsolete protocol failed")); });
    expect(host.querySelector('[data-testid="logic-analyzer-live-decode-error"]')?.textContent).toBe("current decoder failed");
    await act(async () => { changeValue(inputFor(host, "logicAnalyzer.decoder.baud"), "38400"); });
    expect(host.querySelector('[data-testid="logic-analyzer-live-decode-error"]')).toBeNull();
    await act(async () => { action(host, "logicAnalyzer.stopStream").click(); frame.resolve(null); });
    sigrokMocks.readCaptureFrame.mockResolvedValueOnce(dataFrame()).mockReturnValue(deferred<null>().promise);
    await act(async () => { action(host, "logicAnalyzer.startStream").click(); });
    await act(async () => { await vi.advanceTimersByTimeAsync(150); });
    await act(async () => { await vi.advanceTimersByTimeAsync(450); });
    expect(host.querySelector('[data-logic-annotation]')).not.toBeNull();
    await act(async () => { oldStream.reject(new Error("obsolete stream failed")); });
    expect(host.querySelector('[data-testid="logic-analyzer-live-decode-error"]')).toBeNull();
    expect(host.querySelector('[data-logic-annotation]')).not.toBeNull();
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
