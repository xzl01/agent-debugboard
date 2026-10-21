// @vitest-environment jsdom
import { describe, expect, it, vi } from "vitest";
import { act } from "react";
import { createRoot } from "react-dom/client";
import type { Terminal as RealTerminal } from "@xterm/xterm";
import { LanguageProvider } from "@/lib/i18n";
import { j4ReadySnapshot } from "@/lib/j4Serial.testFixtures";
import type { J4SerialSession } from "@/lib/j4SerialSession";
import { SerialTerminalPane } from "./SerialTerminalPane";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

describe("real xterm observer response boundary", () => {
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
      const head = partialCsi ? "\u001b[6" : "cached\u001b[6n\u001b[6";
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
