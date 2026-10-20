// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { SerialTerminalPane } from "./SerialTerminalPane";
import { J4SerialSession } from "@/lib/j4SerialSession";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
const xterm = vi.hoisted(() => ({ terminals: [] as Record<string, any>[], addons: [] as Record<string, any>[], fit: vi.fn(), delay: "none" as "none" | "callback" | "write" }));
vi.mock("@/lib/i18n", () => ({ useI18n: () => ({ t: (key: string) => key }) }));
vi.mock("@xterm/xterm", () => ({ Terminal: class {
  options: Record<string, unknown>; rows = 24;
  decoder = new TextDecoder(); escape = ""; rendered = ""; ansiCommands: string[] = [];
  disposed = false; pending: (() => void)[] = [];
  private parse(data: string | Uint8Array) {
    if (this.disposed) return;
    const text = typeof data === "string" ? data : this.decoder.decode(data, { stream: true });
    for (const char of text) {
      if (this.escape) {
        this.escape += char;
        if (this.escape.length > 2 && /[@-~]/u.test(char)) { this.ansiCommands.push(this.escape); this.escape = ""; }
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
  loadAddon = vi.fn(); open = vi.fn(); getSelection = () => "";
  dataDispose = vi.fn(); binaryDispose = vi.fn();
  data: ((text: string) => void) | null = null;
  binary: ((text: string) => void) | null = null;
  constructor(options: Record<string, unknown>) { this.options = options; xterm.terminals.push(this); }
  onData(fn: (text: string) => void) { this.data = fn; return { dispose: this.dataDispose }; }
  onBinary(fn: (text: string) => void) { this.binary = fn; return { dispose: this.binaryDispose }; }
} }));
vi.mock("@xterm/addon-fit", () => ({ FitAddon: class {
  fit = xterm.fit; dispose = vi.fn();
  constructor() { xterm.addons.push(this); }
} }));

const state = (extra = {}) => ({ generation: 1, state: "ready", dtr: false, read_only: false,
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
let fetchMock: ReturnType<typeof vi.fn>;
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
async function connect(session: J4SerialSession) {
  await act(async () => { await session.connect(); });
  const socket = Socket.instances.at(-1)!;
  act(() => { socket.readyState = 1; socket.onopen?.(); socket.frame({ type: "serial_state", ...state() }); });
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
