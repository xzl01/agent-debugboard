// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { SerialTerminalPane } from "./SerialTerminalPane";
import { J4SerialSession } from "@/lib/j4SerialSession";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
const xterm = vi.hoisted(() => ({ terminals: [] as Record<string, any>[], addons: [] as Record<string, any>[], fit: vi.fn(), delay: "none" as "none" | "callback" | "write" }));
vi.mock("@/lib/i18n", () => ({ useI18n: () => ({
  t: (key: string, values?: Record<string, unknown>) => key === "serial.autoResponder" ? `Auto replies: ${values?.responder}` : key,
}) }));
vi.mock("@xterm/xterm", () => ({ Terminal: class {
  options: Record<string, unknown>; rows = 24;
  modes = { bracketedPasteMode: false };
  decoder = new TextDecoder(); escape = ""; rendered = ""; ansiCommands: string[] = [];
  queryInputDisabled: boolean[] = [];
  disposed = false; pending: (() => void)[] = [];
  private parse(data: string | Uint8Array) {
    if (this.disposed) return;
    const text = typeof data === "string" ? data : this.decoder.decode(data, { stream: true });
    for (const char of text) {
      if (char === "\u0018") this.escape = "";
      else if (this.escape) {
        this.escape += char;
        if (this.escape.length > 2 && /[@-~]/u.test(char)) {
          this.ansiCommands.push(this.escape);
          const reply = this.escape === "\u001b[6n" ? "\u001b[1;1R" : this.escape === "\u001b[5n" ? "\u001b[0n" : this.escape === "\u001b[c" ? "\u001b[?1;2c" : null;
          if (reply) {
            this.queryInputDisabled.push(this.options.disableStdin === true);
            if (!this.options.disableStdin) this.data?.(reply);
          }
          this.escape = "";
        }
      } else if (char === "\u001b") this.escape = char;
      else this.rendered += char;
    }
  }
  write = vi.fn((data: string | Uint8Array, callback?: () => void) => {
    const finish = () => { this.parse(data); callback?.(); };
    if (xterm.delay === "write" && data instanceof Uint8Array) this.pending.push(finish);
    else { this.parse(data); if (xterm.delay === "callback" && callback) this.pending.push(callback); else callback?.(); }
  });
  completeNext() { this.pending.shift()?.(); }
  reset = vi.fn(); clear = vi.fn(); dispose = vi.fn(() => { this.disposed = true; }); refresh = vi.fn(); focus = vi.fn();
  loadAddon = vi.fn(); getSelection = () => "";
  textarea: HTMLTextAreaElement | null = null;
  paste = vi.fn((event: ClipboardEvent) => {
    event.stopPropagation();
    const prepared = (event.clipboardData?.getData("text/plain") ?? "").replace(/\r?\n/g, "\r");
    const payload = this.modes.bracketedPasteMode && this.options.ignoreBracketedPasteMode !== true ? `\u001b[200~${prepared}\u001b[201~` : prepared;
    if (!this.options.disableStdin) this.data?.(payload);
    if (this.textarea) this.textarea.value = "";
  });
  composition = vi.fn((event: CompositionEvent) => {
    if (!this.options.disableStdin) this.data?.(event.data);
  });
  open = vi.fn((host: HTMLElement) => {
    this.textarea = document.createElement("textarea");
    this.textarea.readOnly = Boolean(this.options.disableStdin);
    this.textarea.addEventListener("paste", this.paste);
    this.textarea.addEventListener("compositionend", this.composition);
    host.append(this.textarea);
  });
  dataDispose = vi.fn(); binaryDispose = vi.fn(); keyDispose = vi.fn();
  data: ((text: string) => void) | null = null;
  binary: ((text: string) => void) | null = null;
  key: ((event: { key: string; domEvent: KeyboardEvent }) => void) | null = null;
  constructor(options: Record<string, unknown>) {
    this.options = new Proxy(options, { set: (target, key, value) => {
      target[key as string] = value;
      if (key === "disableStdin" && this.textarea) this.textarea.readOnly = Boolean(value);
      return true;
    } });
    xterm.terminals.push(this);
  }
  onData(fn: (text: string) => void) { this.data = fn; return { dispose: this.dataDispose }; }
  onBinary(fn: (text: string) => void) { this.binary = fn; return { dispose: this.binaryDispose }; }
  onKey(fn: (event: { key: string; domEvent: KeyboardEvent }) => void) { this.key = fn; return { dispose: this.keyDispose }; }
  userKey(text: string, domEvent = new KeyboardEvent("keydown")) {
    this.key?.({ key: text, domEvent });
    if (!this.options.disableStdin) this.data?.(text);
  }
} }));
vi.mock("@xterm/addon-fit", () => ({ FitAddon: class {
  fit = xterm.fit; dispose = vi.fn();
  constructor() { xterm.addons.push(this); }
} }));

const state = (extra = {}) => ({ generation: 1, state: "ready", dtr: false, read_only: false, terminal_responder: true,
  line_coding: { baud: 115200, data_bits: 8, parity: "none", stop_bits: 1, flow_control: "none" }, ...extra });
class Socket {
  static OPEN = 1; static instances: Socket[] = [];
  readyState = 0; bufferedAmount = 0; binaryType = "blob";
  onopen: (() => void) | null = null;
  onmessage: ((event: { data: unknown }) => void) | null = null;
  onclose: (() => void) | null = null; onerror = null;
  sent: (Uint8Array | string)[] = [];
  constructor(public url: string) { Socket.instances.push(this); }
  send(bytes: Uint8Array | string) { this.sent.push(bytes); }
  close = vi.fn(() => { this.readyState = 3; this.onclose?.(); });
  frame(frame: unknown) { this.onmessage?.({ data: JSON.stringify(frame) }); }
}
let root: Root | null;
let host: HTMLDivElement;
let fetchMock: ReturnType<typeof vi.fn<(url: string, init?: RequestInit) => Promise<Response>>>;
let sessions: J4SerialSession[];
// TextEncoder is Node-provided in jsdom; model a browser-realm WS ArrayBuffer.
function rxBuffer(text: string) { return Uint8Array.from(new TextEncoder().encode(text)).buffer; }
async function flush() { await act(async () => { await new Promise((resolve) => setTimeout(resolve, 5)); }); }
async function mount() {
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  const session = new J4SerialSession(); sessions.push(session);
  act(() => root!.render(<SerialTerminalPane session={session} fillHeight />));
  await flush(); return session;
}
async function connect(session: J4SerialSession, reported = state()) {
  await act(async () => { await session.connect(); });
  const socket = Socket.instances.at(-1)!;
  act(() => { socket.readyState = 1; socket.onopen?.(); socket.frame({ type: "serial_state", ...reported }); });
  return socket;
}
beforeEach(() => {
  xterm.terminals = []; xterm.addons = []; xterm.delay = "none"; Socket.instances = []; sessions = []; vi.clearAllMocks(); root = null;
  vi.stubGlobal("WebSocket", Socket);
  vi.stubGlobal("ResizeObserver", class { observe = vi.fn(); disconnect = vi.fn(); });
  vi.stubGlobal("requestAnimationFrame", (callback: FrameRequestCallback) => setTimeout(() => callback(0), 0));
  vi.stubGlobal("cancelAnimationFrame", clearTimeout);
  let id = 0;
  fetchMock = vi.fn(async (url: string, init?: RequestInit) => new Response(JSON.stringify(
    url.endsWith("/serial/j4") ? state() : init?.method === "POST"
      ? { session_id: ++id, ws_url: `/api/v1/live-sessions/${id}/ws` } : { ok: true })));
  vi.stubGlobal("fetch", fetchMock);
});
afterEach(async () => {
  act(() => root?.unmount()); host?.remove(); await Promise.all(sessions.map((session) => session.disconnect())); vi.unstubAllGlobals();
});

describe("J4 xterm surface", () => {
  function openConfig() {
    act(() => (host.querySelector('button[aria-label="J4 serial.configuration"]') as HTMLButtonElement).click());
    return host.querySelector<HTMLInputElement>('input[aria-label="J4 serial.baud"]')!;
  }
  function typeBaud(input: HTMLInputElement, value: string) {
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(input, value);
      input.dispatchEvent(new Event("input", { bubbles: true }));
    });
  }
  function finishWrite(socket: Socket, id: number) {
    const bytes = (socket.sent.at(-1) as Uint8Array).length;
    act(() => {
      socket.frame({ type: "serial_write_accepted", event_id: id, bytes });
      socket.frame({ type: "serial_write_complete", event_id: id, disposition: "completed" });
    });
  }
  function paste(term: Record<string, any>, text: string) {
    const event = new Event("paste", { bubbles: true, cancelable: true });
    Object.defineProperty(event, "clipboardData", { value: { getData: () => text } });
    act(() => term.textarea.dispatchEvent(event));
    return event;
  }
  function compose(term: Record<string, any>, text: string) {
    const event = new CompositionEvent("compositionend", { bubbles: true, cancelable: true, data: text });
    act(() => term.textarea.dispatchEvent(event));
    return event;
  }

  it("displays an authoritative current 5200 baud as a valid editable value and retains five presets", async () => {
    const reported = state({ line_coding: { ...state().line_coding, baud: 5200 } });
    const original = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => url.endsWith("/serial/j4")
      ? Promise.resolve(new Response(JSON.stringify(reported))) : original(url, init));
    const session = await mount(); await connect(session, reported);
    const input = openConfig();
    expect(input.value).toBe("5200");
    expect(input.getAttribute("aria-invalid")).not.toBe("true");
    expect([input.min, input.max, input.step]).toEqual(["4800", "115200", "1"]);
    expect(input.disabled).toBe(false);
    expect([...input.list!.querySelectorAll("option")].map((option) => option.value)).toEqual(
      ["9600", "19200", "38400", "57600", "115200"],
    );
    expect(host.querySelector('[data-testid="serial-terminal-j4"] > div')?.textContent).toContain((5200).toLocaleString());
    expect(host.textContent).toContain("serial.generation 1");
  });

  it("submits custom 5200 with expected_generation and waits for authoritative GET completion", async () => {
    const session = await mount(); await connect(session);
    const input = openConfig(); typeBaud(input, "5200");
    const original = fetchMock.getMockImplementation()!;
    let reported = state();
    fetchMock.mockImplementation((url: string, init?: RequestInit) => init?.method === "PUT"
      ? Promise.resolve(new Response(JSON.stringify({ ok: true, accepted: true, request_id: 7 }), { status: 202 }))
      : url.endsWith("/serial/j4") ? Promise.resolve(new Response(JSON.stringify(reported))) : original(url, init));
    act(() => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })));
    await flush();
    expect(fetchMock).toHaveBeenCalledWith("/api/v1/serial/j4/config", expect.objectContaining({ method: "PUT",
      body: JSON.stringify({ ...state().line_coding, baud: 5200, expected_generation: 1 }) }));
    expect(session.getState().configuring).toBe(true);
    expect(session.getState().serial?.config.baud).toBe(115200);
    expect(host.querySelector('[data-testid="serial-terminal-j4"] > div')?.textContent).toContain((115200).toLocaleString());
    expect(host.textContent).toContain("serial.generation 1");
    reported = state({ generation: 2, line_coding: { ...state().line_coding, baud: 5200 }, config_request: { id: 7, done: true, result: 0 } });
    await act(async () => { await new Promise((resolve) => setTimeout(resolve, 280)); });
    expect(session.getState().configuring).toBe(false);
    expect(session.getState().serial?.config.baud).toBe(5200);
    expect(input.value).toBe("5200");
    expect(host.querySelector('[data-testid="serial-terminal-j4"] > div')?.textContent).toContain((5200).toLocaleString());
    expect(host.textContent).toContain("serial.generation 2");
    expect(fetchMock.mock.calls.filter(([, init]) => init?.method === "PUT")).toHaveLength(1);
  });

  it.each(["", "4799", "115201", "5200.5"])("blocks invalid custom baud %j before PUT, even on programmatic submit", async (value) => {
    const session = await mount(); await connect(session);
    const input = openConfig(); typeBaud(input, value);
    expect(input.value).toBe(value);
    expect(input.getAttribute("aria-invalid")).toBe("true");
    expect(host.querySelector<HTMLButtonElement>('button[type="submit"]')?.disabled).toBe(true);
    expect(host.querySelector('[role="alert"]')?.textContent).toBe("serial.invalidBaud");
    act(() => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })));
    await flush();
    expect(fetchMock.mock.calls.some(([, init]) => init?.method === "PUT")).toBe(false);
    for (const valid of ["4800", "5200", "115200"]) {
      typeBaud(input, valid);
      expect(input.getAttribute("aria-invalid")).not.toBe("true");
      expect(host.querySelector<HTMLButtonElement>('button[type="submit"]')?.disabled).toBe(false);
    }
  });

  it("stays offline until requested and preserves fit, accessible terminal and touch targets", async () => {
    await mount();
    expect(fetchMock).not.toHaveBeenCalled();
    expect(host.querySelector('[role="application"]')?.getAttribute("aria-label")).toBe("J4 serial.terminalAria");
    expect(xterm.fit).toHaveBeenCalled();
    expect(xterm.terminals[0].options.screenReaderMode).toBe(true);
    expect(xterm.terminals[0].options.disableStdin).toBe(true);
    expect([...host.querySelectorAll('button[aria-label]')].every((button) => button.className.includes("h-10"))).toBe(true);
    expect(host.querySelectorAll('[role="application"]')).toHaveLength(1);
    expect(host.querySelector('[data-testid="terminal-engine"]')?.textContent).toBe("xterm.js · MCU UART");
    expect(xterm.terminals[0].open).toHaveBeenCalledWith(host.querySelector('[role="application"]'));
    expect(host.textContent).toContain("serial.connectToInput");
  });

  it("keeps the same xterm canvas and engine label visible after disconnect", async () => {
    const session = await mount();
    const canvas = host.querySelector('[role="application"]');
    await connect(session);
    const terminal = xterm.terminals.at(-1)!;
    const count = xterm.terminals.length;
    await act(async () => { await session.disconnect(); });
    expect(host.querySelector('[role="application"]')).toBe(canvas);
    expect(xterm.terminals).toHaveLength(count);
    expect(terminal.open).toHaveBeenCalledTimes(1);
    expect(terminal.options.disableStdin).toBe(true);
    expect(host.querySelector('[data-testid="terminal-engine"]')?.textContent).toBe("xterm.js · MCU UART");
    expect(host.textContent).toContain("serial.connectToInput");
  });

  it("writes raw RX Uint8Arrays to xterm and UTF-8 text/binary input to the board", async () => {
    const ref = await mount(); const socket = await connect(ref);
    const term = xterm.terminals.at(-1)!; expect(term.options.disableStdin).toBe(false);
    const first = new Uint8Array([0, 255, 0xe4]); const second = new Uint8Array([0xb8, 0xad]);
    act(() => { socket.onmessage?.({ data: first.buffer }); socket.onmessage?.({ data: second.buffer }); });
    await flush(); await flush();
    expect(term.write.mock.calls.filter(([data]: [unknown]) => data instanceof Uint8Array).flatMap(([data]: [Uint8Array]) => [...data]))
      .toEqual([0, 255, 0xe4, 0xb8, 0xad]);
    act(() => { term.data("\0中"); term.binary("\0\xff"); });
    expect([...(socket.sent[1] as Uint8Array)]).toEqual([0, 0xe4, 0xb8, 0xad]);
    expect(socket.sent).toHaveLength(2); // Binary input waits for physical completion.
    act(() => {
      for (let event_id = 1; event_id <= 2; event_id++) {
        socket.frame({ type: "serial_write_accepted", event_id, bytes: event_id === 1 ? 4 : 2 });
        socket.frame({ type: "serial_write_complete", event_id, disposition: "completed" });
      }
    });
    expect([...(socket.sent[2] as Uint8Array)]).toEqual([0, 255]);
    await flush(); expect(host.textContent).toContain("TX 6");
  });

  it("shows suspension/fault/read-only, loss, shared DTR and async configuration states", async () => {
    const ref = await mount(); const socket = await connect(ref);
    act(() => socket.frame({ type: "serial_state", ...state({ state: "paused" }) }));
    expect(host.textContent).toContain("serial.suspended"); expect(xterm.terminals.at(-1)!.options.disableStdin).toBe(true);
    act(() => socket.frame({ type: "serial_state", ...state({ state: "faulted" }) }));
    expect(host.textContent).toContain("serial.faulted");
    act(() => socket.frame({ type: "serial_state", ...state({ read_only: true }) }));
    expect(host.textContent).toContain("serial.readOnly");
    act(() => socket.frame({ type: "serial_overrun", topic: "serial.j4", dropped_bytes: 9, total_dropped_bytes: 9 }));
    expect(host.textContent).toContain("serial.dropped");
    act(() => socket.frame({ type: "serial_state", ...state({ dtr: true }) }));
    act(() => (host.querySelector('button[aria-label="J4 serial.configuration"]') as HTMLButtonElement).click());
    expect(host.textContent).toContain("serial.dtrActive"); expect(host.textContent).toContain("serial.sharedConfig");
    act(() => socket.frame({ type: "serial_state", ...state({ state: "reconfig" }) }));
    expect(host.textContent).toContain("serial.configuring");
  });

  it.each([
    [{ dtr: true, read_only: false, terminal_responder: true }, "serial.responder.cdc"],
    [{ terminal_responder: false }, "serial.responder.other"],
    [{ terminal_responder: false, responder: { transport: "websocket", slot: 4 } }, "serial.responder.websocket #4"],
    [{ terminal_responder: { transport: "raw_tcp", slot: 2 } }, "serial.responder.raw_tcp #2"],
  ])("makes nonresponder %j suppress DSR/DA/CPR immediately while keeping explicit keys and paste enabled", async (responderState, responderLabel) => {
    const session = await mount(); const socket = await connect(session);
    const old = xterm.terminals.at(-1)!; const count = xterm.terminals.length;
    let term = old;
    const raw = "RX\u001b[5n\u001b[6n\u001b[c";
    act(() => {
      socket.frame({ type: "serial_state", ...state(responderState) });
      term = xterm.terminals.at(-1)!;
      // This assertion is inside the frame callback turn, before React effects.
      expect(term.options.disableStdin).toBe(true);
      socket.onmessage?.({ data: rxBuffer(raw) });
    });
    await flush();
    expect(term.queryInputDisabled).toEqual([true, true, true]);
    expect(term.rendered).toBe("RX");
    expect(term.ansiCommands).toEqual(["\u001b[5n", "\u001b[6n", "\u001b[c"]);
    const written = term.write.mock.calls.filter(([data]: [unknown]) => data instanceof Uint8Array);
    expect([...written[0][0]]).toEqual([...new TextEncoder().encode(raw)]);
    expect(socket.sent).toHaveLength(1); // Subscription only, no automatic response TX.
    expect(session.getState().serial?.read_only).toBe(false);
    expect(host.textContent).toContain("serial.observer");
    expect(host.textContent).toContain("serial.manualAllowed");
    expect(host.textContent).not.toContain("serial.readOnly");
    expect(host.querySelector('[data-testid="serial-terminal-responder"]')?.textContent).toContain(responderLabel);
    expect(xterm.terminals).toHaveLength(count);
    expect(old.dispose).not.toHaveBeenCalled();
    expect(term.textarea.readOnly).toBe(false);
    act(() => { term.userKey("x"); term.data("must-not-double-send"); term.binary("ignored"); });
    expect(socket.sent).toHaveLength(2);
    expect([...(socket.sent[1] as Uint8Array)]).toEqual([120]);
    finishWrite(socket, 1); await flush();
    const pasteEvent = paste(term, "hello\n");
    expect(pasteEvent.defaultPrevented).toBe(true);
    expect(term.paste).not.toHaveBeenCalled();
    expect(socket.sent).toHaveLength(3);
    expect([...(socket.sent[2] as Uint8Array)]).toEqual([...new TextEncoder().encode("hello\r")]);
    finishWrite(socket, 2); await flush();
    const ctrlC = [...host.querySelectorAll<HTMLButtonElement>("button")].find((button) => button.textContent === "Ctrl-C")!;
    expect(ctrlC.disabled).toBe(false);
    act(() => ctrlC.click());
    expect(socket.sent).toHaveLength(4);
    expect([...(socket.sent[3] as Uint8Array)]).toEqual([3]);
    finishWrite(socket, 3); await flush();
    act(() => {
      socket.frame({ type: "serial_state", ...state({ terminal_responder: true, read_only: false, dtr: false }) });
      term = xterm.terminals.at(-1)!;
      expect(term.options.disableStdin).toBe(true); // Promotion fence not drained yet.
      socket.onmessage?.({ data: rxBuffer("\u001b[6n") });
    });
    await flush(); await flush();
    expect(term.options.disableStdin).toBe(false);
    expect(term.queryInputDisabled).toEqual([true, true, true, false]);
    expect(socket.sent).toHaveLength(5);
    expect([...(socket.sent[4] as Uint8Array)]).toEqual([...new TextEncoder().encode("\u001b[1;1R")]);
    expect(host.querySelector('[data-testid="serial-terminal-responder"]')?.textContent).toContain("serial.responder.thisClient");
  });

  it("uses only onData/onBinary for an active responder, with no duplicate onKey or paste path", async () => {
    const session = await mount(); const socket = await connect(session);
    const term = xterm.terminals.at(-1)!;
    expect(term.options.disableStdin).toBe(false);
    act(() => term.userKey("a"));
    expect(socket.sent).toHaveLength(2);
    expect([...(socket.sent[1] as Uint8Array)]).toEqual([97]);
    finishWrite(socket, 1); await flush();
    const event = paste(term, "b");
    expect(event.defaultPrevented).toBe(false);
    expect(term.paste).toHaveBeenCalledTimes(1);
    expect(socket.sent).toHaveLength(3);
    expect([...(socket.sent[2] as Uint8Array)]).toEqual([98]);
    finishWrite(socket, 2); await flush();
    act(() => term.binary("\xff"));
    expect(socket.sent).toHaveLength(4);
    expect([...(socket.sent[3] as Uint8Array)]).toEqual([255]);
  });

  it.each([false, true])("sends a Chinese composition exactly once with responder=%s", async (responder) => {
    const session = await mount(); const socket = await connect(session, state({ terminal_responder: responder }));
    const term = xterm.terminals.at(-1)!;
    if (!responder) {
      act(() => term.userKey("candidate", new KeyboardEvent("keydown", { isComposing: true, keyCode: 229 })));
      expect(socket.sent).toHaveLength(1);
    }
    term.textarea.value = "中文输入";
    const event = compose(term, "中文输入");
    expect(socket.sent).toHaveLength(2);
    expect([...(socket.sent[1] as Uint8Array)]).toEqual([...new TextEncoder().encode("中文输入")]);
    expect(term.composition).toHaveBeenCalledTimes(responder ? 1 : 0);
    expect(event.defaultPrevented).toBe(!responder);
    if (!responder) expect(term.textarea.value).toBe("");
    expect(term.options.disableStdin).toBe(!responder);
  });

  it("captures promotion-pending IME before xterm can schedule a second send after the CAN callback", async () => {
    const session = await mount(); const socket = await connect(session, state({ terminal_responder: false }));
    const term = xterm.terminals.at(-1)!;
    const send = vi.spyOn(session, "writeManual");
    term.composition.mockImplementation((event: CompositionEvent) => {
      setTimeout(() => { if (!term.options.disableStdin) term.data(event.data); }, 0);
    });
    xterm.delay = "callback";
    act(() => socket.frame({ type: "serial_state", ...state({ terminal_responder: true }) }));
    await flush();
    expect(term.pending).toHaveLength(1);
    expect([...(term.write.mock.calls.at(-1)![0] as Uint8Array)]).toEqual([0x18]);
    expect(term.options.disableStdin).toBe(true);
    term.textarea.value = "中";
    const event = compose(term, "中");
    expect(send.mock.calls).toEqual([["中"]]);
    finishWrite(socket, 1);
    act(() => { xterm.delay = "none"; term.completeNext(); });
    expect(term.options.disableStdin).toBe(false);
    await flush(); await flush();
    expect(send.mock.calls).toEqual([["中"]]);
    expect(socket.sent).toHaveLength(2);
    expect(term.composition).not.toHaveBeenCalled();
    expect(event.defaultPrevented).toBe(true);
    expect(term.textarea.value).toBe("");
    send.mockRestore();
  });

  it.each([false, true].flatMap((responder) => [false, true].flatMap((bracketed) =>
    ["cr", "crlf", "lf"].map((ending) => ({ responder, bracketed, ending }))))
  )("matches exact xterm paste preparation with $ending, bracketed=$bracketed, responder=$responder", async ({ responder, bracketed, ending }) => {
    const session = await mount(); const socket = await connect(session, state({ terminal_responder: responder }));
    const term = xterm.terminals.at(-1)!;
    term.modes.bracketedPasteMode = bracketed;
    term.textarea.value = "screen-reader scratch text";
    const select = host.querySelector<HTMLSelectElement>('[aria-label="J4 serial.lineEnding"]')!;
    act(() => { select.value = ending; select.dispatchEvent(new Event("change", { bubbles: true })); });
    const event = paste(term, "中文\r\nA\nB\rC");
    const newline = { cr: "\r", crlf: "\r\n", lf: "\n" }[ending]!;
    const text = `中文${newline}A${newline}B${newline}C`;
    const expected = bracketed ? `\u001b[200~${text}\u001b[201~` : text;
    expect(socket.sent).toHaveLength(2);
    expect([...(socket.sent[1] as Uint8Array)]).toEqual([...new TextEncoder().encode(expected)]);
    expect(event.defaultPrevented).toBe(!responder);
    expect(term.textarea.value).toBe("");
    expect(term.paste).toHaveBeenCalledTimes(responder ? 1 : 0);
  });

  it("honors xterm ignoreBracketedPasteMode in the passive capture path", async () => {
    const session = await mount(); const socket = await connect(session, state({ terminal_responder: false }));
    const term = xterm.terminals.at(-1)!;
    term.modes.bracketedPasteMode = true;
    term.options.ignoreBracketedPasteMode = true;
    paste(term, "A\r\nB\n");
    expect(socket.sent).toHaveLength(2);
    expect([...(socket.sent[1] as Uint8Array)]).toEqual([...new TextEncoder().encode("A\rB\r")]);
  });

  it("keeps xterm bracket markers even for an empty passive paste", async () => {
    const session = await mount(); const socket = await connect(session, state({ terminal_responder: false }));
    const term = xterm.terminals.at(-1)!;
    term.modes.bracketedPasteMode = true;
    paste(term, "");
    expect(socket.sent).toHaveLength(2);
    expect([...(socket.sent[1] as Uint8Array)]).toEqual([...new TextEncoder().encode("\u001b[200~\u001b[201~")]);
  });

  it.each(["write", "callback", "raf"] as const)("promotion drains passive %s/query state behind a barrier without replay or an extra CPR", async (delay) => {
    const session = await mount(); const socket = await connect(session, state({ terminal_responder: false }));
    const old = xterm.terminals.at(-1)!; const oldFit = xterm.addons.at(-1)!;
    const removeListener = vi.spyOn(old.textarea.parentElement, "removeEventListener");
    old.modes.bracketedPasteMode = true;
    const prefix = "old\u001b[6";
    const queued = "n\u001b[6n";
    xterm.delay = delay === "raf" ? "none" : delay;
    act(() => socket.onmessage?.({ data: rxBuffer(prefix) }));
    if (delay !== "raf") {
      await flush();
      expect(old.pending).toHaveLength(1);
    }
    act(() => {
      socket.onmessage?.({ data: rxBuffer(queued) });
      xterm.delay = "none";
      socket.frame({ type: "serial_state", ...state({ terminal_responder: true }) });
      old.completeNext(); // Deliberately late old parser/write completion.
      socket.onmessage?.({ data: rxBuffer("nNEW") });
    });
    await flush(); await flush(); await flush(); await flush();
    const fresh = xterm.terminals.at(-1)!;
    expect(fresh).toBe(old);
    expect(old.dispose).not.toHaveBeenCalled();
    expect(oldFit.dispose).not.toHaveBeenCalled();
    expect(old.keyDispose).not.toHaveBeenCalled();
    expect(old.reset).not.toHaveBeenCalled();
    for (const type of ["paste", "compositionend"]) expect(removeListener.mock.calls.some(([event]) => event === type)).toBe(false);
    expect(fresh.rendered).toBe("oldnNEW");
    expect(fresh.queryInputDisabled).toEqual([true, true]);
    expect(fresh.modes.bracketedPasteMode).toBe(true);
    expect(fresh.textarea.readOnly).toBe(false);
    expect(socket.sent).toHaveLength(1);
    const cached = new TextEncoder().encode(prefix + queued + "nNEW").length;
    expect(host.textContent).toContain(`serial.logCached ${cached} B`);
    expect(session.getState().rxBytes).toBe(cached);
    act(() => socket.onmessage?.({ data: rxBuffer("\u001b[6n") }));
    await flush();
    expect(socket.sent).toHaveLength(2); // Exactly one CPR, for the new query only.
    expect([...(socket.sent[1] as Uint8Array)]).toEqual([...new TextEncoder().encode("\u001b[1;1R")]);
    removeListener.mockRestore();
  });

  it("demotion keeps the same parser/modes passive before new RX and keeps explicit IME/paste once", async () => {
    const session = await mount(); const socket = await connect(session);
    const old = xterm.terminals.at(-1)!;
    old.modes.bracketedPasteMode = true;
    xterm.delay = "callback";
    act(() => socket.onmessage?.({ data: rxBuffer("old\u001b[6") }));
    await flush();
    act(() => {
      xterm.delay = "none";
      socket.frame({ type: "serial_state", ...state({ terminal_responder: false }) });
      const passive = xterm.terminals.at(-1)!;
      expect(passive).toBe(old);
      expect(passive.options.disableStdin).toBe(true);
      old.completeNext();
      socket.onmessage?.({ data: rxBuffer("nNEW\u001b[6n") });
    });
    await flush();
    const passive = xterm.terminals.at(-1)!;
    expect(old.dispose).not.toHaveBeenCalled();
    expect(passive.rendered).toBe("oldNEW");
    expect(passive.queryInputDisabled).toEqual([true, true]);
    expect(passive.modes.bracketedPasteMode).toBe(true);
    expect(passive.textarea.readOnly).toBe(false);
    expect(socket.sent).toHaveLength(1);
    compose(passive, "中");
    expect(socket.sent).toHaveLength(2);
    finishWrite(socket, 1); await flush();
    paste(passive, "A\n");
    expect(socket.sent).toHaveLength(3);
    const same = xterm.terminals.length;
    act(() => socket.frame({ type: "serial_state", ...state({ terminal_responder: false }) }));
    expect(xterm.terminals).toHaveLength(same); // No rebuild without a transition.
  });

  it("keeps nonresponders explicit input locked by automation without enabling automatic replies", async () => {
    const session = await mount(); const socket = await connect(session, state({ terminal_responder: false }));
    const term = xterm.terminals.at(-1)!;
    act(() => session.setAutomationActive(true));
    act(() => term.userKey("x")); paste(term, "paste");
    expect(term.options.disableStdin).toBe(true);
    expect(socket.sent).toHaveLength(1);
    act(() => session.setAutomationActive(false));
    act(() => term.userKey("y"));
    expect(term.options.disableStdin).toBe(true); // Auto replies stay off.
    expect(socket.sent).toHaveLength(2);
    expect([...(socket.sent[1] as Uint8Array)]).toEqual([121]);
  });

  it("keeps RX and explicit input available after a generic write rejection without an ownership lock", async () => {
    const session = await mount(); const socket = await connect(session);
    const term = xterm.terminals.at(-1)!;
    act(() => term.userKey("a"));
    act(() => socket.frame({ type: "error", topic: "serial.j4", code: "write_rejected", message: "write not admitted" }));
    await flush();
    expect(term.options.disableStdin).toBe(false);
    expect(session.getState().serial?.read_only).toBe(false);
    act(() => socket.onmessage?.({ data: rxBuffer("RX") }));
    await flush();
    expect(term.rendered).toBe("RX");
    expect(session.getState().phase).toBe("connected");
    expect(socket.close).not.toHaveBeenCalled();
    act(() => term.userKey("b"));
    expect(socket.sent).toHaveLength(3);
    expect([...(socket.sent[2] as Uint8Array)]).toEqual([98]);
  });

  it("allows two Web terminals to type while only the selected terminal answers target CSI6n", async () => {
    host = document.createElement("div"); document.body.append(host); root = createRoot(host);
    const first = new J4SerialSession(); const second = new J4SerialSession(); sessions.push(first, second);
    act(() => root!.render(<><SerialTerminalPane key="first" session={first} /></>));
    await flush();
    act(() => root!.render(<><SerialTerminalPane key="first" session={first} /><SerialTerminalPane key="second" session={second} /></>));
    await flush();
    const a = await connect(first, state({ terminal_responder: true }));
    const b = await connect(second, state({ terminal_responder: false }));
    const active = xterm.terminals.filter((term) => !term.disposed);
    expect(active).toHaveLength(2);
    act(() => { a.onmessage?.({ data: rxBuffer("\u001b[6n") }); b.onmessage?.({ data: rxBuffer("\u001b[6n") }); });
    await flush();
    expect(a.sent).toHaveLength(2);
    expect(b.sent).toHaveLength(1);
    finishWrite(a, 1); await flush();
    act(() => { active[0].userKey("A"); active[1].userKey("B"); });
    expect(a.sent).toHaveLength(3);
    expect(b.sent).toHaveLength(2);
    expect([...(a.sent[2] as Uint8Array)]).toEqual([65]);
    expect([...(b.sent[1] as Uint8Array)]).toEqual([66]);
  });

  it("replaces the terminal synchronously at a generation boundary without dropping following-generation bytes", async () => {
    const session = await mount(); const socket = await connect(session);
    const old = xterm.terminals.at(-1)!;
    act(() => {
      socket.frame({ type: "serial_state", ...state({ generation: 2 }) });
      socket.onmessage?.({ data: new Uint8Array([0, 255, 65]).buffer });
    });
    await flush();
    expect(old.dispose).toHaveBeenCalledTimes(1);
    expect(old.reset).not.toHaveBeenCalled();
    const writes = xterm.terminals.at(-1)!.write.mock.calls.filter(([data]: [unknown]) => data instanceof Uint8Array);
    expect(writes.flatMap(([data]: [Uint8Array]) => [...data])).toEqual([0, 255, 65]);
  });

  it.each([
    ["UTF-8", [0xe4, 0xb8], [0xad, 78, 69, 87], "�NEW"],
    ["ANSI", [27, 91, 51, 49], [109, 78, 69, 87], "mNEW"],
  ] as const)("isolates split %s state and ignores a disposed terminal's delayed write callback", async (_kind, head, tail, expected) => {
    const session = await mount(); const socket = await connect(session);
    const old = xterm.terminals.at(-1)!; const oldFit = xterm.addons.at(-1)!;
    xterm.delay = "callback";
    act(() => socket.onmessage?.({ data: Uint8Array.from(head).buffer }));
    await flush();
    expect(old.pending).toHaveLength(1);
    act(() => {
      socket.frame({ type: "serial_state", ...state({ generation: 2 }) });
      socket.onmessage?.({ data: Uint8Array.from(tail).buffer });
    });
    const fresh = xterm.terminals.at(-1)!;
    expect(fresh).not.toBe(old);
    expect(old.dispose).toHaveBeenCalledTimes(1);
    expect(oldFit.dispose).toHaveBeenCalledTimes(1);
    expect(old.dataDispose).toHaveBeenCalledTimes(1);
    expect(old.binaryDispose).toHaveBeenCalledTimes(1);
    expect(old.keyDispose).toHaveBeenCalledTimes(1);
    expect(old.reset).not.toHaveBeenCalled();
    await flush();
    expect(fresh.rendered).toBe(expected);
    expect(fresh.ansiCommands).toEqual([]);
    expect(fresh.write).toHaveBeenCalledTimes(1);
    act(() => socket.onmessage?.({ data: rxBuffer("NEXT") }));
    const sentBefore = socket.sent.length;
    act(() => { old.completeNext(); old.data("stale input"); old.binary("x"); });
    await flush();
    // The old completion cannot release the fresh renderer's in-flight lock.
    expect(fresh.write).toHaveBeenCalledTimes(1);
    expect(socket.sent).toHaveLength(sentBefore);
    act(() => fresh.completeNext());
    await flush();
    expect(fresh.write).toHaveBeenCalledTimes(2);
    expect(fresh.rendered).toBe(`${expected}NEXT`);
  });

  it("drops old queued parser writes and RAF bytes instead of replaying them into a new generation", async () => {
    const session = await mount(); const socket = await connect(session);
    const old = xterm.terminals.at(-1)!;
    xterm.delay = "write";
    act(() => socket.onmessage?.({ data: new Uint8Array([27, 91, 51, 49]).buffer }));
    await flush();
    expect(old.pending).toHaveLength(1);
    // This second chunk is queued behind the old in-flight parser write.
    act(() => socket.onmessage?.({ data: rxBuffer("OLD") }));
    act(() => {
      socket.frame({ type: "serial_state", ...state({ generation: 2 }) });
      socket.onmessage?.({ data: rxBuffer("mNEW") });
    });
    const fresh = xterm.terminals.at(-1)!;
    act(() => old.completeNext());
    await flush();
    expect(old.dispose).toHaveBeenCalledTimes(1);
    expect(old.reset).not.toHaveBeenCalled();
    expect(fresh.write).toHaveBeenCalledTimes(1);
    act(() => fresh.completeNext());
    expect(fresh.rendered).toBe("mNEW");
    expect(fresh.ansiCommands).toEqual([]);
  });

  it("cancels an old generation RAF and keeps user Clear separate from terminal recreation", async () => {
    const session = await mount(); const socket = await connect(session);
    const old = xterm.terminals.at(-1)!;
    act(() => {
      socket.onmessage?.({ data: rxBuffer("OLD") });
      socket.frame({ type: "serial_state", ...state({ generation: 2 }) });
      socket.onmessage?.({ data: rxBuffer("NEW") });
    });
    await flush();
    const fresh = xterm.terminals.at(-1)!;
    expect(old.write).not.toHaveBeenCalled();
    expect(fresh.rendered).toBe("NEW");
    const count = xterm.terminals.length;
    act(() => (host.querySelector('button[aria-label="J4 serial.clear"]') as HTMLButtonElement).click());
    expect(xterm.terminals).toHaveLength(count);
    expect(fresh.clear).toHaveBeenCalledTimes(1);
    expect(fresh.write).toHaveBeenCalledWith("\u001b[2J\u001b[H");
    expect(fresh.dispose).not.toHaveBeenCalled();
  });

  it("keeps the shared backend alive when only the renderer unmounts and cleans up on owner disconnect", async () => {
    const session = await mount(); const socket = await connect(session);
    act(() => root!.unmount()); root = null; await flush();
    expect(socket.close).not.toHaveBeenCalled();
    expect(session.isConnected("j4")).toBe(true);
    await session.disconnect();
    expect(socket.close).toHaveBeenCalledOnce();
    expect(fetchMock.mock.calls.some(([url, init]) => url === "/api/v1/live-sessions/1" && init?.method === "DELETE")).toBe(true);
    fetchMock.mockResolvedValue(new Response(JSON.stringify({ ok: false }), { status: 404 }));
    const next = await mount(); await act(async () => { await next.connect(); });
    expect(host.textContent).toContain("serial.unavailable"); expect(host.textContent).toContain("serial.unavailableHint");
    expect(xterm.terminals.at(-1)?.options.disableStdin).toBe(true);
  });
});
