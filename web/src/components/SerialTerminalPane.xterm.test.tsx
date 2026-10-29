// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { act } from "react";
import { createRoot } from "react-dom/client";
import type { Terminal as RealTerminal } from "@xterm/xterm";
import { LanguageProvider } from "@/lib/i18n";
import { j4ReadySnapshot } from "@/lib/j4Serial.testFixtures";
import type { J4SerialSession } from "@/lib/j4SerialSession";
import { SerialTerminalPane } from "./SerialTerminalPane";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

function resizeProbe(limit = 9999): string {
  return `\u001b7\u001b[r\u001b[${limit};${limit}H\u001b[6n\u001b8`;
}

let fontsDescriptor: PropertyDescriptor | undefined;
let rootFontFamily: string | undefined;
beforeEach(() => {
  fontsDescriptor = Object.getOwnPropertyDescriptor(document, "fonts");
  rootFontFamily = document.documentElement.dataset.terminalFontFamily;
  const boundingRect = HTMLElement.prototype.getBoundingClientRect;
  vi.spyOn(HTMLElement.prototype, "getBoundingClientRect").mockImplementation(function (this: HTMLElement) {
    if (!this.classList.contains("terminal-host")) return boundingRect.call(this);
    return this.closest("[hidden]") ? new DOMRect() : new DOMRect(0, 0, 1056, 688);
  });
});
afterEach(() => {
  if (fontsDescriptor) Object.defineProperty(document, "fonts", fontsDescriptor);
  else Reflect.deleteProperty(document, "fonts");
  if (rootFontFamily === undefined) delete document.documentElement.dataset.terminalFontFamily;
  else document.documentElement.dataset.terminalFontFamily = rootFontFamily;
  vi.restoreAllMocks(); vi.unstubAllGlobals();
});

async function mountLayoutFixture(hidden = false) {
  vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(null);
  // Load both renderer modules before mounting. The component imports them
  // together; a cold addon import can outlive a fixed parser-settling timer
  // when this file runs alongside the complete component suite.
  const [{ Terminal }] = await Promise.all([import("@xterm/xterm"), import("@xterm/addon-fit")]);
  const originalOpen = Terminal.prototype.open;
  const terminals: RealTerminal[] = [];
  vi.spyOn(Terminal.prototype, "open").mockImplementation(function (this: RealTerminal, mountPoint) {
    mountPoint.style.width = "1056px"; mountPoint.style.height = "688px";
    originalOpen.call(this, mountPoint);
    this.element!.style.padding = "0px";
    terminals.push(this);
  });
  const frames = new Map<number, FrameRequestCallback>(); let id = 0;
  vi.stubGlobal("requestAnimationFrame", (callback: FrameRequestCallback) => { frames.set(++id, callback); return id; });
  vi.stubGlobal("cancelAnimationFrame", (frame: number) => { frames.delete(frame); });
  const observers: (() => void)[] = [];
  vi.stubGlobal("ResizeObserver", class {
    constructor(callback: ResizeObserverCallback) { observers.push(() => callback([], this as unknown as ResizeObserver)); }
    observe() {} disconnect() {}
  });
  vi.stubGlobal("matchMedia", () => ({ matches: false, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {} }));
  const snapshot = j4ReadySnapshot(); const bytes = new Set<(data: Uint8Array) => void>();
  const send = vi.fn(async () => {});
  const session = { getState: () => snapshot, subscribeState: () => () => {},
    subscribeBytes: (listener: (data: Uint8Array) => void) => { bytes.add(listener); return () => { bytes.delete(listener); }; },
    subscribeClear: () => () => {}, writeManual: send, writeBinaryManual: vi.fn(async () => {}),
  } as unknown as J4SerialSession;
  const host = document.createElement("div"); host.hidden = hidden; document.body.append(host); const root = createRoot(host);
  const settle = async () => { await act(async () => { await new Promise((resolve) => setTimeout(resolve, 10)); }); };
  const flushFrame = () => act(() => {
    const callbacks = [...frames.values()]; frames.clear(); callbacks.forEach((callback) => callback(0));
  });
  localStorage.setItem("lang", "en");
  await act(async () => { root.render(<LanguageProvider><SerialTerminalPane session={session} /></LanguageProvider>); });
  await settle();
  return { host, terminals, frames, get term() { return terminals.at(-1)!; }, send, settle, flushFrame,
    resize: () => act(() => observers.forEach((callback) => callback())),
    receive: async (text: string) => {
      act(() => bytes.forEach((listener) => listener(new TextEncoder().encode(text))));
      flushFrame(); await settle();
    },
    close: () => { act(() => root.unmount()); host.remove(); localStorage.clear(); },
  };
}

describe("real xterm observer response boundary", () => {
  it("recovers an initially hidden DOM renderer's zero glyph cache through public resize before fitting", async () => {
    vi.stubGlobal("OffscreenCanvas", undefined);
    const offsetWidth = Object.getOwnPropertyDescriptor(HTMLElement.prototype, "offsetWidth")!.get!;
    const offsetHeight = Object.getOwnPropertyDescriptor(HTMLElement.prototype, "offsetHeight")!.get!;
    vi.spyOn(HTMLElement.prototype, "offsetWidth", "get").mockImplementation(function (this: HTMLElement) {
      if (!this.classList.contains("xterm-char-measure-element")) return offsetWidth.call(this);
      return this.closest("[hidden]") ? 0 : 16 * (this.textContent?.length ?? 0);
    });
    vi.spyOn(HTMLElement.prototype, "offsetHeight", "get").mockImplementation(function (this: HTMLElement) {
      if (!this.classList.contains("xterm-char-measure-element")) return offsetHeight.call(this);
      return this.closest("[hidden]") ? 0 : 16;
    });
    const view = await mountLayoutFixture(true);
    try {
      view.flushFrame();
      expect([view.term.cols, view.term.rows]).toEqual([80, 24]);
      act(() => { view.host.hidden = false; });
      view.resize(); view.flushFrame();
      expect([view.term.cols, view.term.rows]).toEqual([65, 38]);
      const screen = view.term.element!.querySelector<HTMLElement>(".xterm-screen")!;
      expect(Number.parseFloat(screen.style.width) / view.term.cols).toBe(16);
      expect(view.send).not.toHaveBeenCalled();
    } finally { view.close(); }
  });

  it.each(["observer", "visibility event"])("retains alternate-screen dimensions while an ancestor is hidden and refits after show via %s", async (path) => {
    const { FitAddon } = await import("@xterm/addon-fit");
    vi.spyOn(document, "visibilityState", "get").mockReturnValue("visible");
    let visibleSize = { cols: 105, rows: 34 };
    const dimensions = vi.spyOn(FitAddon.prototype, "proposeDimensions").mockImplementation(function () {
      return document.querySelector("[hidden] .terminal-host") ? { cols: 8, rows: 5 } : visibleSize;
    });
    const view = await mountLayoutFixture();
    try {
      view.flushFrame(); await view.receive("\u001b[?1049h\u001b[34;100HKEEP");
      expect([view.term.cols, view.term.rows]).toEqual([105, 34]);
      const marker = () => view.term.buffer.active.getLine(33)?.translateToString(true).trim();
      expect(marker()).toBe("KEEP");
      const calls = dimensions.mock.calls.length;
      const mountPoint = view.host.querySelector<HTMLElement>(".terminal-host")!;
      act(() => { view.host.hidden = true; mountPoint.style.width = "100%"; mountPoint.style.height = "100%"; });
      view.resize(); view.flushFrame();
      expect(document.visibilityState).toBe("visible");
      expect(dimensions).toHaveBeenCalledTimes(calls);
      expect([view.term.cols, view.term.rows]).toEqual([105, 34]);
      expect(marker()).toBe("KEEP");
      act(() => { view.host.hidden = false; visibleSize = { cols: 120, rows: 40 }; });
      if (path === "observer") view.resize();
      else act(() => document.dispatchEvent(new Event("visibilitychange")));
      view.flushFrame();
      expect([view.term.cols, view.term.rows]).toEqual([120, 40]);
      expect(marker()).toBe("KEEP");
      expect(view.send).not.toHaveBeenCalled();
    } finally { view.close(); }
  });

  it("uses loaded glyph metrics for initial creation and later font-family changes without rebuilding the parser", async () => {
    let glyphWidth = 10; const measuredWidths: number[] = [];
    const offsetWidth = Object.getOwnPropertyDescriptor(HTMLElement.prototype, "offsetWidth")!.get!;
    const offsetHeight = Object.getOwnPropertyDescriptor(HTMLElement.prototype, "offsetHeight")!.get!;
    vi.stubGlobal("OffscreenCanvas", undefined); // Exercise xterm's real DOM measurement fallback.
    vi.spyOn(HTMLElement.prototype, "offsetWidth", "get").mockImplementation(function (this: HTMLElement) {
      if (!this.classList.contains("xterm-char-measure-element")) return offsetWidth.call(this);
      measuredWidths.push(glyphWidth); return glyphWidth * (this.textContent?.length ?? 0);
    });
    vi.spyOn(HTMLElement.prototype, "offsetHeight", "get").mockImplementation(function (this: HTMLElement) {
      return this.classList.contains("xterm-char-measure-element") ? 16 : offsetHeight.call(this);
    });
    let fontsReady!: () => void;
    const ready = new Promise<void>((resolve) => { fontsReady = resolve; });
    const load = vi.fn(() => ready.then(() => []));
    Object.defineProperty(document, "fonts", { configurable: true, value: { status: "loading", ready, load } });
    document.documentElement.dataset.terminalFontFamily = '"DelayedMono", monospace';
    const view = await mountLayoutFixture();
    try {
      await view.receive("RX");
      expect(view.terminals).toHaveLength(0);
      expect(measuredWidths).toEqual([]);
      expect(load).toHaveBeenCalledWith('13px "DelayedMono", monospace', "W");
      await act(async () => { glyphWidth = 16; fontsReady(); await Promise.resolve(); });
      await view.settle(); view.flushFrame(); await view.settle();
      expect(view.terminals).toHaveLength(1);
      expect(measuredWidths.length).toBeGreaterThan(0);
      expect(measuredWidths.every((width) => width === 16)).toBe(true);
      // Real CharSizeService -> DomRenderer -> FitAddon. A stale 10px cell
      // would yield 104 columns here, not 65; no _core or fit-result override.
      expect([view.term.cols, view.term.rows]).toEqual([65, 38]);
      const screen = view.term.element!.querySelector<HTMLElement>(".xterm-screen")!;
      expect(Number.parseFloat(screen.style.width) / view.term.cols).toBe(16);
      expect(view.term.buffer.active.getLine(0)?.translateToString(true)).toBe("RX");
      await view.receive("\u001b[?2004h");
      let nextFontReady!: () => void;
      const nextReady = new Promise<void>((resolve) => { nextFontReady = resolve; });
      Object.defineProperty(document, "fonts", { configurable: true, value: {
        status: "loading", ready: nextReady, load: vi.fn(() => nextReady.then(() => [])),
      } });
      act(() => { document.documentElement.dataset.terminalFontFamily = '"NextLoadedMono", monospace'; });
      await view.settle();
      expect(view.term.options.fontFamily).toBe('"DelayedMono", monospace');
      expect(view.term.cols).toBe(65);
      await act(async () => { glyphWidth = 20; nextFontReady(); await Promise.resolve(); });
      await view.settle(); view.flushFrame();
      expect(view.term.options.fontFamily).toBe('"NextLoadedMono", monospace');
      expect(view.term.cols).toBe(52);
      expect(Number.parseFloat(screen.style.width) / view.term.cols).toBe(20);
      expect(view.terminals).toHaveLength(1);
      expect(view.term.modes.bracketedPasteMode).toBe(true);
      expect(view.term.buffer.active.getLine(0)?.translateToString(true)).toBe("RX");
      expect(view.send).not.toHaveBeenCalled();
    } finally { view.close(); }
  });

  it("defers loaded font and size changes while hidden, then measures the latest request without losing the alternate screen", async () => {
    vi.stubGlobal("OffscreenCanvas", undefined);
    vi.spyOn(document, "visibilityState", "get").mockReturnValue("visible");
    const offsetWidth = Object.getOwnPropertyDescriptor(HTMLElement.prototype, "offsetWidth")!.get!;
    const offsetHeight = Object.getOwnPropertyDescriptor(HTMLElement.prototype, "offsetHeight")!.get!;
    const fontSize = (element: HTMLElement) => Number.parseFloat(element.style.fontSize || element.parentElement?.style.fontSize || "13px");
    vi.spyOn(HTMLElement.prototype, "offsetWidth", "get").mockImplementation(function (this: HTMLElement) {
      if (!this.classList.contains("xterm-char-measure-element")) return offsetWidth.call(this);
      if (this.closest("[hidden]")) return 0; // Real DOM measuring while hidden retains a previous nonzero cache.
      const family = this.style.fontFamily || this.parentElement?.style.fontFamily || "";
      const width = family.includes("LoadedWide") ? 20 : 10;
      return width * fontSize(this) / 13 * (this.textContent?.length ?? 0);
    });
    vi.spyOn(HTMLElement.prototype, "offsetHeight", "get").mockImplementation(function (this: HTMLElement) {
      if (!this.classList.contains("xterm-char-measure-element")) return offsetHeight.call(this);
      return this.closest("[hidden]") ? 0 : Math.round(16 * fontSize(this) / 13);
    });
    let slowFontReady!: () => void;
    const slowReady = new Promise<void>((resolve) => { slowFontReady = resolve; });
    const load = vi.fn(() => slowReady.then(() => []));
    Object.defineProperty(document, "fonts", { configurable: true, value: {
      status: "loaded", check: (font: string) => !font.includes("SlowMono"), load, ready: Promise.resolve(),
    } });
    document.documentElement.dataset.terminalFontFamily = '"LoadedNarrow", monospace';
    const view = await mountLayoutFixture();
    try {
      view.flushFrame(); await view.receive("\u001b[?1049h\u001b[34;1HKEEP");
      expect([view.term.cols, view.term.rows]).toEqual([104, 38]);
      const screen = view.term.element!.querySelector<HTMLElement>(".xterm-screen")!;
      const marker = () => view.term.buffer.active.getLine(33)?.translateToString(true);
      expect(Number.parseFloat(screen.style.width) / view.term.cols).toBe(10);
      act(() => {
        view.host.hidden = true;
        document.documentElement.dataset.terminalFontFamily = '"LoadedWide", monospace';
      });
      await view.settle(); view.flushFrame(); view.flushFrame();
      expect(view.term.options.fontFamily).toBe('"LoadedNarrow", monospace');
      expect([view.term.cols, view.term.rows]).toEqual([104, 38]);
      expect(marker()).toBe("KEEP");
      expect(view.frames.size).toBe(0); // Hidden styles do not schedule an animation loop.
      act(() => { view.host.hidden = false; });
      view.resize(); view.flushFrame();
      expect(view.term.options.fontFamily).toBe('"LoadedWide", monospace');
      expect([view.term.cols, view.term.rows]).toEqual([52, 38]);
      expect(Number.parseFloat(screen.style.width) / view.term.cols).toBe(20);
      expect(marker()).toBe("KEEP");

      // Font-size changes use the same guarded path, not an effect-time setter.
      act(() => {
        view.host.hidden = true;
        (view.host.querySelector('button[aria-label="J4 Increase terminal font"]') as HTMLButtonElement).click();
      });
      view.flushFrame();
      expect(view.term.options.fontSize).toBe(13);
      expect([view.term.cols, view.term.rows]).toEqual([52, 38]);
      expect(marker()).toBe("KEEP");
      act(() => { view.host.hidden = false; document.dispatchEvent(new Event("fullscreenchange")); });
      view.flushFrame();
      expect(view.term.options.fontSize).toBe(14);
      expect(view.term.cols).toBe(Math.floor((1056 - 14) / (20 * 14 / 13)));
      expect(Number.parseFloat(screen.style.width) / view.term.cols).toBeCloseTo(20 * 14 / 13, 1);
      expect(marker()).toBe("KEEP");

      // An older font promise must not overwrite a newer already-loaded face.
      act(() => { view.host.hidden = true; document.documentElement.dataset.terminalFontFamily = '"SlowMono", monospace'; });
      await view.settle(); view.flushFrame();
      expect(load).toHaveBeenCalledWith('14px "SlowMono", monospace', "W");
      act(() => { document.documentElement.dataset.terminalFontFamily = '"LoadedWide", monospace'; });
      await view.settle(); view.flushFrame();
      act(() => { view.host.hidden = false; window.dispatchEvent(new Event("resize")); });
      view.flushFrame();
      await act(async () => { slowFontReady(); await Promise.resolve(); });
      await view.settle(); view.flushFrame();
      expect(view.term.options.fontFamily).toBe('"LoadedWide", monospace');
      expect(view.term.options.fontSize).toBe(14);
      expect(marker()).toBe("KEEP");
      expect(view.terminals).toHaveLength(1);
      expect(view.send).not.toHaveBeenCalled();
    } finally { view.close(); }
  });

  it("measures an unpadded content-sized parent with the real addon-fit geometry calculation", async () => {
    const { FitAddon } = await import("@xterm/addon-fit");
    // jsdom has no layout: supply the browser-resolved pixel boxes and glyph
    // metrics, not a mocked proposeDimensions result or a column correction.
    const frame = document.createElement("div");
    frame.style.cssText = "box-sizing:border-box;width:1072px;height:704px;padding:8px";
    const mountPoint = document.createElement("div");
    mountPoint.style.cssText = "box-sizing:border-box;width:1056px;height:688px;padding:0;border:0";
    const element = document.createElement("div");
    element.style.padding = "0px";
    document.body.append(frame); frame.append(element);
    const fit = new FitAddon();
    fit.activate({ element, options: { scrollback: 0 },
      _core: { _renderService: { dimensions: { css: { cell: { width: 8, height: 16 } } } } },
    } as unknown as RealTerminal);
    try {
      // The old padded border-box parent overstates both visible dimensions.
      expect(fit.proposeDimensions()).toEqual({ cols: 134, rows: 44 });
      frame.append(mountPoint); mountPoint.append(element);
      expect(fit.proposeDimensions()).toEqual({ cols: 132, rows: 43 });
    } finally {
      fit.dispose(); frame.remove();
    }
  });

  it("answers DA as CSI c without resetting the terminal or its modes", async () => {
    const canvas = vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(null);
    const { Terminal } = await import("@xterm/xterm");
    const term = new Terminal({ cols: 132, rows: 43 });
    const replies: string[] = [];
    const subscription = term.onData((data) => replies.push(data));
    try {
      await new Promise<void>((resolve) => term.write("\u001b[?2004hKEEP\u001b[c", resolve));
      expect(replies).toHaveLength(1);
      expect(replies[0]).toMatch(/^\u001b\[\?.*c$/u);
      expect(term.buffer.active.getLine(0)?.translateToString(true)).toBe("KEEP");
      expect(term.modes.bracketedPasteMode).toBe(true);
      expect([term.cols, term.rows]).toEqual([132, 43]);
    } finally {
      subscription.dispose(); term.dispose(); canvas.mockRestore();
    }
  });

  it.each([999, 9999])("reports current character dimensions for the standard %i resize probe and CSI 18t", async (limit) => {
    const canvas = vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(null);
    const { Terminal } = await import("@xterm/xterm");
    const term = new Terminal({ cols: 132, rows: 43, windowOptions: { getWinSizeChars: true } });
    const replies: string[] = [];
    const subscription = term.onData((data) => replies.push(data));
    const write = (data: string) => new Promise<void>((resolve) => term.write(new TextEncoder().encode(data), resolve));
    const queries = `${resizeProbe(limit)}\u001b[18t`;
    try {
      await write(queries);
      expect(replies).toEqual(["\u001b[43;132R", "\u001b[8;43;132t"]);
      expect([term.buffer.active.cursorY, term.buffer.active.cursorX]).toEqual([0, 0]);
      await write("\u001b[8;60;200t"); // Read-only reports do not enable window-size setters.
      expect([term.cols, term.rows]).toEqual([132, 43]);
      expect(replies).toHaveLength(2);
      replies.length = 0;
      term.resize(96, 28);
      expect(replies).toEqual([]); // Local resize is not UART input.
      await write(queries);
      expect(replies).toEqual(["\u001b[28;96R", "\u001b[8;28;96t"]);
      replies.length = 0;
      term.options.disableStdin = true;
      await write(queries);
      expect(replies).toEqual([]);
    } finally {
      subscription.dispose(); term.dispose(); canvas.mockRestore();
    }
  });

  it.each(["active", "observer", "CDC DTR", "automation", "backpressure"] as const)(
    "fits locally without UART TX and gates real size-query replies in %s mode", async (mode) => {
      const canvas = vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(null);
      const { Terminal } = await import("@xterm/xterm");
      const { FitAddon } = await import("@xterm/addon-fit");
      const originalOpen = Terminal.prototype.open;
      let term!: RealTerminal;
      const open = vi.spyOn(Terminal.prototype, "open").mockImplementation(function (this: RealTerminal, host) {
        originalOpen.call(this, host); term = this;
      });
      // jsdom cannot measure glyphs. Only stub the dimensions measurement;
      // keep real FitAddon.fit/Terminal.resize, parser, and component TX gates.
      const dimensions = vi.spyOn(FitAddon.prototype, "proposeDimensions").mockReturnValue({ cols: 132, rows: 43 });
      const frames = new Map<number, FrameRequestCallback>();
      let frameId = 0;
      vi.stubGlobal("requestAnimationFrame", (callback: FrameRequestCallback) => { frames.set(++frameId, callback); return frameId; });
      vi.stubGlobal("cancelAnimationFrame", (id: number) => { frames.delete(id); });
      vi.stubGlobal("ResizeObserver", class { observe() {} disconnect() {} });
      vi.stubGlobal("matchMedia", () => ({ matches: false, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {} }));
      let snapshot = j4ReadySnapshot({ automationActive: mode === "automation", backpressure: mode === "backpressure" });
      snapshot = { ...snapshot, serial: { ...snapshot.serial!, terminal_responder: mode !== "observer", dtr: mode === "CDC DTR" } };
      const bytes = new Set<(data: Uint8Array) => void>();
      const send = vi.fn(async () => {}); const binarySend = vi.fn(async () => {});
      const session = {
        getState: () => snapshot, subscribeState: () => () => {},
        subscribeBytes: (listener: (data: Uint8Array) => void) => { bytes.add(listener); return () => { bytes.delete(listener); }; },
        subscribeClear: () => () => {}, writeManual: send, writeBinaryManual: binarySend,
      } as unknown as J4SerialSession;
      const settle = async () => { await act(async () => { await new Promise((resolve) => setTimeout(resolve, 10)); }); };
      const flushFrame = () => act(() => {
        const current = [...frames.values()]; frames.clear(); current.forEach((callback) => callback(0));
      });
      const query = async () => {
        act(() => bytes.forEach((listener) => listener(new TextEncoder().encode(`${resizeProbe()}\u001b[18t`))));
        flushFrame(); await settle();
      };
      localStorage.setItem("lang", "en");
      const host = document.createElement("div"); document.body.append(host); const root = createRoot(host);
      try {
        act(() => root.render(<LanguageProvider><SerialTerminalPane session={session} /></LanguageProvider>));
        await settle(); flushFrame(); await settle();
        expect(term.options.windowOptions).toEqual({ getWinSizeChars: true });
        expect([term.cols, term.rows]).toEqual([132, 43]);
        expect(send).not.toHaveBeenCalled(); expect(binarySend).not.toHaveBeenCalled();
        await query();
        expect(send.mock.calls).toEqual(mode === "active" ? [["\u001b[43;132R"], ["\u001b[8;43;132t"]] : []);
        send.mockClear();
        dimensions.mockReturnValue({ cols: 100, rows: 30 });
        act(() => window.dispatchEvent(new Event("resize")));
        flushFrame(); await settle();
        expect([term.cols, term.rows]).toEqual([100, 30]);
        expect(send).not.toHaveBeenCalled(); expect(binarySend).not.toHaveBeenCalled();
        await query();
        expect(send.mock.calls).toEqual(mode === "active" ? [["\u001b[30;100R"], ["\u001b[8;30;100t"]] : []);
        expect(binarySend).not.toHaveBeenCalled();
      } finally {
        act(() => root.unmount()); host.remove(); localStorage.clear();
        open.mockRestore(); dimensions.mockRestore(); canvas.mockRestore(); vi.unstubAllGlobals();
      }
    },
  );

  it("renders unfiltered RX but suppresses automatic DSR/DA/CPR with disableStdin", async () => {
    // This is the real xterm parser, without opening a canvas renderer in jsdom.
    const canvas = vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(null);
    const { Terminal } = await import("@xterm/xterm");
    const term = new Terminal({ disableStdin: true });
    const responses: string[] = [];
    const subscription = term.onData((data) => responses.push(data));
    const queries = "\u001b[5n\u001b[6n\u001b[c";
    const write = (data: string) => new Promise<void>((resolve) => term.write(new TextEncoder().encode(data), resolve));
    try {
      await write(`RX${queries}`);
      expect(term.buffer.active.getLine(0)?.translateToString(true)).toBe("RX");
      expect(responses).toEqual([]);
      term.options.disableStdin = false;
      await write(queries);
      expect(responses).toContain("\u001b[0n");
      expect(responses.some((reply) => /^\u001b\[\d+;\d+R$/u.test(reply))).toBe(true);
      expect(responses.some((reply) => /^\u001b\[\?.*c$/u.test(reply))).toBe(true);
    } finally {
      subscription.dispose(); term.dispose(); canvas.mockRestore();
    }
  });

  it.each(["queued parser", "delayed callback", "queued RAF"].flatMap((delay) => [
    { delay, partialCsi: false }, { delay, partialCsi: true },
  ]))("promotion drains real xterm $delay state (partial CSI: $partialCsi) without losing modes or a stale CPR", async ({ delay, partialCsi }) => {
    const canvas = vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(null);
    const { Terminal } = await import("@xterm/xterm");
    const { FitAddon } = await import("@xterm/addon-fit");
    const terminals: RealTerminal[] = [];
    // Keep the real xterm byte decoder, write buffer and parser. Only omit the
    // DOM/canvas renderer, which jsdom cannot paint or measure.
    const open = vi.spyOn(Terminal.prototype, "open").mockImplementation(function (this: RealTerminal, host) {
      terminals.push(this); host.append(document.createElement("div"));
    });
    const fit = vi.spyOn(FitAddon.prototype, "fit").mockImplementation(() => {});
    const dispose = vi.spyOn(Terminal.prototype, "dispose");
    const originalWrite = Terminal.prototype.write;
    const held: (() => void)[] = [];
    let holdCallback = delay === "delayed callback";
    let holdFence = true;
    const write = vi.spyOn(Terminal.prototype, "write").mockImplementation(function (this: RealTerminal, bytes, callback) {
      originalWrite.call(this, bytes, () => {
        const isFence = bytes instanceof Uint8Array && bytes.length === 1 && bytes[0] === 0x18;
        if ((holdCallback || (holdFence && isFence)) && callback) held.push(callback);
        else callback?.();
      });
    });
    const frames = new Map<number, FrameRequestCallback>();
    let frameId = 0;
    vi.stubGlobal("requestAnimationFrame", (callback: FrameRequestCallback) => { frames.set(++frameId, callback); return frameId; });
    vi.stubGlobal("cancelAnimationFrame", (id: number) => { frames.delete(id); });
    vi.stubGlobal("ResizeObserver", class { observe() {} disconnect() {} });
    let snapshot = j4ReadySnapshot();
    snapshot = { ...snapshot, serial: { ...snapshot.serial!, terminal_responder: false } };
    const states = new Set<() => void>(); const bytes = new Set<(data: Uint8Array) => void>();
    const send = vi.fn(async () => {});
    const session = {
      getState: () => snapshot,
      subscribeState: (listener: () => void) => { states.add(listener); return () => { states.delete(listener); }; },
      subscribeBytes: (listener: (data: Uint8Array) => void) => { bytes.add(listener); return () => { bytes.delete(listener); }; },
      subscribeClear: () => () => {}, writeManual: send, writeBinaryManual: vi.fn(async () => {}),
    } as unknown as J4SerialSession;
    const receive = (text: string) => {
      const chunk = Uint8Array.from(new TextEncoder().encode(text));
      snapshot = { ...snapshot, rxBytes: snapshot.rxBytes + chunk.length };
      states.forEach((listener) => listener()); bytes.forEach((listener) => listener(chunk));
    };
    const flushFrame = () => act(() => {
      const current = [...frames.values()]; frames.clear(); current.forEach((callback) => callback(0));
    });
    const flushParser = async () => { await act(async () => { await new Promise((resolve) => setTimeout(resolve, 10)); }); };
    localStorage.setItem("lang", "en");
    const host = document.createElement("div"); document.body.append(host);
    const root = createRoot(host);
    try {
      act(() => root.render(<LanguageProvider><SerialTerminalPane session={session} /></LanguageProvider>));
      await flushParser(); flushFrame();
      const old = terminals.at(-1)!;
      await new Promise<void>((resolve) => originalWrite.call(old, "\u001b[?2004h", resolve));
      expect(old.modes.bracketedPasteMode).toBe(true);
      const head = "\u001b[18t" + (partialCsi ? "\u001b[6" : "cached\u001b[6n\u001b[6");
      const queued = partialCsi ? "" : "n\u001b[6n";
      act(() => receive(head));
      if (delay !== "queued RAF") flushFrame();
      if (delay === "delayed callback") {
        await flushParser();
        expect(held).toHaveLength(1);
        expect(old.buffer.active.getLine(0)?.translateToString(true)).toBe(partialCsi ? "" : "cached");
      }
      act(() => {
        if (queued) receive(queued);
        snapshot = { ...snapshot, serial: { ...snapshot.serial!, terminal_responder: true } };
        states.forEach((listener) => listener());
        holdCallback = false;
        held.splice(0).forEach((callback) => callback());
        if (!partialCsi) receive("nNEW");
      });
      const fresh = terminals.at(-1)!;
      expect(fresh).toBe(old);
      expect(dispose.mock.contexts).not.toContain(old);
      for (let index = 0; index < 6; index++) { flushFrame(); await flushParser(); }
      expect(fresh.options.disableStdin).toBe(true);
      expect(send).not.toHaveBeenCalled();
      expect(write.mock.calls.filter(([data]) => data instanceof Uint8Array && data.length === 1 && data[0] === 0x18)).toHaveLength(1);
      expect(held).toHaveLength(1);
      act(() => {
        holdFence = false;
        held.splice(0).forEach((callback) => callback());
      });
      expect(fresh.options.disableStdin).toBe(false);
      if (partialCsi) act(() => receive("nNEW"));
      flushFrame(); await flushParser();
      expect(send).not.toHaveBeenCalled();
      expect(fresh.buffer.active.getLine(0)?.translateToString(true)).toBe(partialCsi ? "nNEW" : "cachednNEW");
      expect(fresh.modes.bracketedPasteMode).toBe(true);
      const received = new TextEncoder().encode(head + queued + "nNEW").length;
      expect(host.textContent).toContain(`LOG ${received} B`);
      expect(snapshot.rxBytes).toBe(received);
      act(() => receive("\u001b[6n")); flushFrame(); await flushParser();
      expect(send).toHaveBeenCalledTimes(1);
      expect(send.mock.calls[0]).toEqual([partialCsi ? "\u001b[1;5R" : "\u001b[1;11R"]);
    } finally {
      act(() => root.unmount()); host.remove(); localStorage.clear();
      open.mockRestore(); fit.mockRestore(); dispose.mockRestore(); write.mockRestore(); canvas.mockRestore(); vi.unstubAllGlobals();
    }
  });

  it.each(["passive", "active", "promotion pending"] as const)("sends Chinese composition once with real xterm DOM handlers in %s mode", async (mode) => {
    const canvas = vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(null);
    const { Terminal } = await import("@xterm/xterm");
    const { FitAddon } = await import("@xterm/addon-fit");
    const terminals: RealTerminal[] = [];
    const originalOpen = Terminal.prototype.open;
    // Open the real textarea and CompositionHelper; no input event mocks.
    const open = vi.spyOn(Terminal.prototype, "open").mockImplementation(function (this: RealTerminal, host) {
      originalOpen.call(this, host); terminals.push(this);
    });
    const fit = vi.spyOn(FitAddon.prototype, "fit").mockImplementation(() => {});
    const originalWrite = Terminal.prototype.write;
    let canCallback: (() => void) | undefined;
    const write = vi.spyOn(Terminal.prototype, "write").mockImplementation(function (this: RealTerminal, data, callback) {
      originalWrite.call(this, data, () => {
        if (data instanceof Uint8Array && data.length === 1 && data[0] === 0x18) canCallback = callback;
        else callback?.();
      });
    });
    const frames = new Map<number, FrameRequestCallback>();
    let frameId = 0;
    vi.stubGlobal("requestAnimationFrame", (callback: FrameRequestCallback) => { frames.set(++frameId, callback); return frameId; });
    vi.stubGlobal("cancelAnimationFrame", (id: number) => { frames.delete(id); });
    vi.stubGlobal("ResizeObserver", class { observe() {} disconnect() {} });
    vi.stubGlobal("matchMedia", () => ({ matches: false, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {} }));
    let snapshot = j4ReadySnapshot();
    snapshot = { ...snapshot, serial: { ...snapshot.serial!, terminal_responder: mode === "active" } };
    const states = new Set<() => void>();
    const send = vi.fn(async () => {});
    const session = {
      getState: () => snapshot,
      subscribeState: (listener: () => void) => { states.add(listener); return () => { states.delete(listener); }; },
      subscribeBytes: () => () => {}, subscribeClear: () => () => {},
      writeManual: send, writeBinaryManual: vi.fn(async () => {}),
    } as unknown as J4SerialSession;
    const settle = async () => { await act(async () => { await new Promise((resolve) => setTimeout(resolve, 10)); }); };
    const flushFrame = () => act(() => {
      const current = [...frames.values()]; frames.clear(); current.forEach((callback) => callback(0));
    });
    localStorage.setItem("lang", "en");
    const host = document.createElement("div"); document.body.append(host);
    const root = createRoot(host);
    let dataSubscription: { dispose(): void } | undefined;
    try {
      act(() => root.render(<LanguageProvider><SerialTerminalPane session={session} /></LanguageProvider>));
      await settle();
      const term = terminals.at(-1)!;
      const textarea = term.textarea!;
      const nativeData = vi.fn();
      const nativeEnd = vi.fn();
      dataSubscription = term.onData(nativeData);
      textarea.addEventListener("compositionend", nativeEnd);
      expect(textarea.readOnly).toBe(false);
      if (mode === "promotion pending") {
        act(() => {
          snapshot = { ...snapshot, serial: { ...snapshot.serial!, terminal_responder: true } };
          states.forEach((listener) => listener());
        });
        flushFrame(); await settle();
        expect(canCallback).toBeTypeOf("function");
        expect(term.options.disableStdin).toBe(true);
      }
      const event = new CompositionEvent("compositionend", { bubbles: true, cancelable: true, data: "中" });
      act(() => {
        textarea.dispatchEvent(new CompositionEvent("compositionstart", { bubbles: true }));
        textarea.value = "中";
        textarea.dispatchEvent(new CompositionEvent("compositionupdate", { bubbles: true, data: "中" }));
        textarea.dispatchEvent(event);
      });
      if (mode !== "active") expect(send.mock.calls).toEqual([["中"]]);
      else expect(send).not.toHaveBeenCalled(); // Native xterm composition is deferred.
      if (mode === "promotion pending") {
        act(() => canCallback!());
        expect(term.options.disableStdin).toBe(false);
      }
      await settle(); await settle();
      expect(send.mock.calls).toEqual([["中"]]);
      expect(nativeData.mock.calls).toEqual(mode === "active" ? [["中"]] : []);
      expect(nativeEnd).toHaveBeenCalledTimes(mode === "active" ? 1 : 0);
      expect(event.defaultPrevented).toBe(mode !== "active");
      if (mode !== "active") expect(textarea.value).toBe("");
    } finally {
      dataSubscription?.dispose();
      act(() => root.unmount()); host.remove(); localStorage.clear();
      open.mockRestore(); fit.mockRestore(); write.mockRestore(); canvas.mockRestore(); vi.unstubAllGlobals();
    }
  });
});
