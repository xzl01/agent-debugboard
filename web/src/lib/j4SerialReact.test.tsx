// @vitest-environment jsdom
import { act, StrictMode } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { useJ4Serial, useJ4SerialSnapshot, type J4BoardAvailability } from "./j4SerialReact";
import type { J4SerialSession } from "./j4SerialSession";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
const wireState = { generation: 1, state: "ready", dtr: false, read_only: false,
  line_coding: { baud: 115200, data_bits: 8, parity: "none", stop_bits: 1, flow_control: "none" } };
class Socket {
  static OPEN = 1; static instances: Socket[] = [];
  readyState = 0; bufferedAmount = 0; binaryType = "";
  onopen: (() => void) | null = null;
  onmessage: ((event: { data: unknown }) => void) | null = null;
  onclose: (() => void) | null = null; onerror: (() => void) | null = null;
  sent: (string | Uint8Array)[] = [];
  constructor(public url: string) { Socket.instances.push(this); }
  send(data: string | Uint8Array) { this.sent.push(data); }
  close = vi.fn(() => { this.readyState = 3; this.onclose?.(); });
  open() { this.readyState = 1; this.onopen?.(); this.onmessage?.({ data: JSON.stringify({ type: "serial_state", topic: "serial.j4", ...wireState }) }); }
}
let root: Root | null;
let host: HTMLDivElement;
let session: J4SerialSession;
let fetchMock: ReturnType<typeof vi.fn>;
let phases: string[];
async function flush() { for (let i = 0; i < 8; i++) await act(async () => { await Promise.resolve(); }); }
function Probe({ board }: { board: J4BoardAvailability }) {
  session = useJ4Serial(board);
  phases.push(useJ4SerialSnapshot(session).phase);
  return null;
}
async function render(board: J4BoardAvailability, strict = false) {
  if (!root) { host = document.createElement("div"); document.body.append(host); root = createRoot(host); }
  act(() => root!.render(strict ? <StrictMode><Probe board={board} /></StrictMode> : <Probe board={board} />));
  await flush();
}
beforeEach(() => {
  root = null; phases = []; Socket.instances = []; let id = 0;
  vi.stubGlobal("WebSocket", Socket);
  fetchMock = vi.fn(async (url: string, init?: RequestInit) => new Response(JSON.stringify(
    url.endsWith("/serial/j4") ? wireState : init?.method === "POST"
      ? { session_id: ++id, ws_url: `/api/v1/live-sessions/${id}/ws` } : { ok: true })));
  vi.stubGlobal("fetch", fetchMock);
});
afterEach(async () => { act(() => root?.unmount()); root = null; host?.remove(); await flush(); vi.unstubAllGlobals(); });
const posts = () => fetchMock.mock.calls.filter(([, init]) => init?.method === "POST");
function page(type: "pagehide" | "pageshow", persisted = true) {
  act(() => window.dispatchEvent(new PageTransitionEvent(type, { persisted })));
}

describe("App-owned J4 automatic connection epochs", () => {
  it("waits for board availability, automatically connects once, and publishes Connecting → Connected", async () => {
    await render({ available: false });
    expect(fetchMock).not.toHaveBeenCalled();
    await render({ available: true, uptimeMs: 10000 });
    expect(posts()).toHaveLength(1);
    expect(session.getState().phase).toBe("connecting");
    act(() => Socket.instances[0].open());
    expect(session.getState().phase).toBe("connected");
    expect(phases).toContain("connecting");
    expect(Socket.instances[0].sent).toEqual([JSON.stringify({ type: "subscribe", topic: "serial.j4" })]);
    await render({ available: true, uptimeMs: 12000 });
    expect(posts()).toHaveLength(1);
  });

  it("keeps manual Disconnect sticky through status ticks, while manual Connect still works", async () => {
    await render({ available: true, uptimeMs: 10000 });
    act(() => Socket.instances[0].open());
    await act(async () => { await session.disconnect(); });
    for (let i = 1; i <= 4; i++) await render({ available: true, uptimeMs: 10000 + i * 2000 });
    expect(session.getState().phase).toBe("disconnected");
    expect(posts()).toHaveLength(1);
    await act(async () => { await session.connect(); });
    expect(posts()).toHaveLength(2);
  });

  it.each([
    ["availability", { available: false, uptimeMs: 300 }],
    ["explicit restart", { available: true, uptimeMs: 300, restarting: true }],
  ] as const)("ignores uptime rollback after manual Disconnect and reconnects once only after %s recovery", async (_name, boundary) => {
    await render({ available: true, uptimeMs: 20000 });
    act(() => Socket.instances[0].open());
    await act(async () => { await session.disconnect(); });
    await render({ available: true, uptimeMs: 10000 });
    await render({ available: true, uptimeMs: 200 });
    expect(session.getState().phase).toBe("disconnected");
    expect(posts()).toHaveLength(1);
    await render(boundary);
    await render({ available: true, uptimeMs: 400, restarting: false });
    expect(posts()).toHaveLength(2);
    await render({ available: true, uptimeMs: 100 });
    expect(posts()).toHaveLength(2);
  });

  it("never hot-retries unavailable endpoints, but allows one explicit new availability epoch", async () => {
    fetchMock.mockImplementation(async () => new Response(JSON.stringify({ available: false }), { status: 404 }));
    await render({ available: true, uptimeMs: 10000 });
    expect(session.getState().phase).toBe("unavailable");
    for (let i = 1; i <= 5; i++) await render({ available: true, uptimeMs: 10000 + i * 2000 });
    expect(fetchMock).toHaveBeenCalledTimes(1);
    await render({ available: false });
    await render({ available: true, uptimeMs: 23000 });
    expect(fetchMock).toHaveBeenCalledTimes(2);
    expect(posts()).toHaveLength(0);
  });

  it("does not retry a closed socket within the same epoch", async () => {
    await render({ available: true, uptimeMs: 10000 });
    const socket = Socket.instances[0]; act(() => socket.open());
    act(() => { socket.readyState = 3; socket.onclose?.(); });
    await flush();
    await render({ available: true, uptimeMs: 300 });
    page("pagehide"); page("pageshow"); await flush();
    expect(posts()).toHaveLength(1);
    expect(session.getState().phase).toBe("disconnected");
  });

  it("keeps automatic-response observers connected without reconnecting or blocking manual TX", async () => {
    await render({ available: true, uptimeMs: 10000 });
    const socket = Socket.instances[0]; act(() => socket.open());
    act(() => socket.onmessage?.({ data: JSON.stringify({ type: "serial_state", topic: "serial.j4", ...wireState, terminal_responder: false, dtr: true }) }));
    for (let i = 0; i < 4; i++) await render({ available: true, uptimeMs: 12000 + i * 2000 });
    expect(session.getState().phase).toBe("connected");
    expect(session.getState().serial?.read_only).toBe(false);
    expect(session.getState().serial?.terminal_responder).toBe(false);
    expect(posts()).toHaveLength(1);
    expect(socket.close).not.toHaveBeenCalled();
    act(() => socket.onmessage?.({ data: JSON.stringify({ type: "serial_state", topic: "serial.j4", ...wireState, terminal_responder: true }) }));
    expect(session.getState().serial?.read_only).toBe(false);
    expect(session.getState().error).toBeNull();
    expect(posts()).toHaveLength(1);
  });

  it("reconnects once after a reboot epoch and clears old TX without deleting released WS sessions", async () => {
    await render({ available: true, uptimeMs: 10000 });
    const old = Socket.instances[0]; act(() => old.open());
    let write!: Promise<void>; act(() => { write = session.writeManual("old TX"); });
    const rejected = expect(write).rejects.toMatchObject({ code: "disconnected" });
    await render({ available: true, uptimeMs: 300, restarting: true });
    await render({ available: true, uptimeMs: 400, restarting: false });
    await rejected;
    expect(posts()).toHaveLength(2);
    expect(old.close).toHaveBeenCalledTimes(1);
    const next = Socket.instances[1]; act(() => next.open());
    expect(next.sent).toEqual([JSON.stringify({ type: "subscribe", topic: "serial.j4" })]);
    await render({ available: true, uptimeMs: 2300 });
    expect(posts()).toHaveLength(2);
    act(() => root!.unmount()); root = null; await flush();
    const deletions = fetchMock.mock.calls.filter(([, init]) => init?.method === "DELETE").map(([url]) => url);
    expect(deletions).toEqual([]);
  });

  it("starts only once under StrictMode and once more after an explicit reboot recovery", async () => {
    await render({ available: true, uptimeMs: 10000 }, true);
    expect(posts()).toHaveLength(1);
    await render({ available: true, uptimeMs: 12000, restarting: true }, true);
    await render({ available: true, uptimeMs: 100, restarting: false }, true);
    expect(posts()).toHaveLength(2);
  });

  it.each([false, true])("synchronously suspends on pagehide and restores one fresh subscription (StrictMode=%s)", async (strict) => {
    await render({ available: true }, strict);
    const old = Socket.instances[0]; act(() => old.open());
    const stale = { message: old.onmessage!, close: old.onclose!, error: old.onerror! };
    const failures = [vi.fn(), vi.fn(), vi.fn()];
    let writes!: Promise<unknown>[];
    act(() => { writes = [session.writeManual("manual"), session.writeBinaryManual(new Uint8Array([255])), session.write("automation")]
      .map((write, index) => write.catch(failures[index])); });
    page("pagehide");
    expect(session.getState().phase).toBe("disconnected");
    expect(old.close).toHaveBeenCalledTimes(1);
    await expect(session.writeManual("too late")).rejects.toMatchObject({ code: "read_only" });
    page("pageshow"); page("pageshow"); await flush(); await Promise.all(writes);
    failures.forEach((failure) => { expect(failure).toHaveBeenCalledTimes(1); expect(failure.mock.calls[0][0]).toMatchObject({ code: "disconnected" }); });
    expect(posts()).toHaveLength(2);
    const next = Socket.instances[1]; act(() => next.open());
    expect(next.url).not.toBe(old.url);
    expect(next.sent).toEqual([JSON.stringify({ type: "subscribe", topic: "serial.j4" })]);
    act(() => {
      stale.close(); stale.error();
      stale.message({ data: JSON.stringify({ type: "serial_state", ...wireState, generation: 99 }) });
      stale.message({ data: new Uint8Array([255]).buffer });
    });
    expect(session.getState()).toMatchObject({ phase: "connected", serial: { generation: 1 }, rxBytes: 0, pendingWrites: 0, error: null });
  });

  it("does not interpret ordinary hidden visibility or initial pageshow as suspension", async () => {
    await render({ available: true }); const socket = Socket.instances[0]; act(() => socket.open());
    page("pageshow", false);
    const visibility = vi.spyOn(document, "visibilityState", "get").mockReturnValue("hidden");
    act(() => document.dispatchEvent(new Event("visibilitychange"))); await flush();
    expect(socket.close).not.toHaveBeenCalled(); expect(posts()).toHaveLength(1);
    visibility.mockRestore();
    page("pagehide", false); page("pageshow", false); await flush();
    expect(socket.close).toHaveBeenCalledTimes(1); expect(posts()).toHaveLength(1);
  });

  it("deduplicates freeze/pagehide and resume/pageshow, waiting for close and board recovery", async () => {
    await render({ available: true }); const old = Socket.instances[0]; act(() => old.open());
    old.close.mockImplementation(() => { old.readyState = 2; });
    act(() => document.dispatchEvent(new Event("freeze"))); page("pagehide");
    expect(old.close).toHaveBeenCalledTimes(1);
    await render({ available: false });
    act(() => document.dispatchEvent(new Event("resume"))); page("pageshow"); await flush();
    expect(posts()).toHaveLength(1);
    await render({ available: true }); expect(posts()).toHaveLength(1);
    act(() => { old.readyState = 3; old.onclose?.(); }); await flush();
    expect(posts()).toHaveLength(2);
    page("pageshow"); act(() => document.dispatchEvent(new Event("resume"))); await flush();
    expect(posts()).toHaveLength(2);
  });

  it("preserves restore intent across two BFCache cycles while the old socket is closing", async () => {
    await render({ available: true }); const old = Socket.instances[0]; act(() => old.open());
    old.close.mockImplementation(() => { old.readyState = 2; });
    page("pagehide"); page("pageshow"); await flush();
    expect(session.getState().phase).toBe("disconnected"); expect(posts()).toHaveLength(1);
    page("pagehide"); page("pageshow"); await flush();
    expect(posts()).toHaveLength(1); expect(old.close).toHaveBeenCalledTimes(1);
    act(() => { old.readyState = 3; old.onclose?.(); }); await flush();
    expect(posts()).toHaveLength(2); expect(Socket.instances).toHaveLength(2);
    const next = Socket.instances[1]; act(() => next.open());
    expect(next.sent).toEqual([JSON.stringify({ type: "subscribe", topic: "serial.j4" })]);
    page("pageshow"); await render({ available: true, uptimeMs: 10000 });
    expect(posts()).toHaveLength(2); expect(session.getState().phase).toBe("connected");
  });

  it("preserves restore intent through a second freeze/resume while waiting for board availability", async () => {
    await render({ available: true }); act(() => Socket.instances[0].open());
    page("pagehide"); await render({ available: false });
    page("pageshow"); await flush();
    expect(session.getState().phase).toBe("disconnected"); expect(posts()).toHaveLength(1);
    act(() => document.dispatchEvent(new Event("freeze")));
    act(() => document.dispatchEvent(new Event("resume"))); await flush();
    expect(posts()).toHaveLength(1);
    await render({ available: true });
    expect(posts()).toHaveLength(2); expect(Socket.instances).toHaveLength(2);
    act(() => Socket.instances[1].open());
    await render({ available: true, uptimeMs: 10000 });
    expect(posts()).toHaveLength(2); expect(session.getState().phase).toBe("connected");
  });

  it("lets manual Disconnect cancel the second BFCache restore queued behind socket close", async () => {
    await render({ available: true }); const old = Socket.instances[0]; act(() => old.open());
    old.close.mockImplementation(() => { old.readyState = 2; });
    page("pagehide"); page("pageshow"); await flush();
    page("pagehide"); page("pageshow"); await flush();
    let disconnected!: Promise<void>;
    act(() => { disconnected = session.disconnect(); });
    expect(posts()).toHaveLength(1);
    act(() => { old.readyState = 3; old.onclose?.(); }); await disconnected; await flush();
    page("pagehide"); page("pageshow"); await render({ available: true, uptimeMs: 10000 });
    expect(posts()).toHaveLength(1); expect(Socket.instances).toHaveLength(1);
    expect(old.close).toHaveBeenCalledTimes(1); expect(session.getState().phase).toBe("disconnected");
  });

  it.each(["before", "suspended", "queued", "closing"])("manual Disconnect wins %s a BFCache restore", async (when) => {
    await render({ available: true }); const old = Socket.instances[0]; act(() => old.open());
    if (when === "before") await act(async () => { await session.disconnect(); });
    if (when === "closing") old.close.mockImplementation(() => { old.readyState = 2; });
    page("pagehide");
    let disconnected: Promise<void> | undefined;
    if (when === "suspended") act(() => { disconnected = session.disconnect(); });
    page("pageshow");
    if (when === "closing") await flush();
    if (when === "queued" || when === "closing") act(() => { disconnected = session.disconnect(); });
    if (when === "closing") act(() => { old.readyState = 3; old.onclose?.(); });
    await disconnected; await flush();
    expect(posts()).toHaveLength(1); expect(session.getState().phase).toBe("disconnected");
  });

  it("restores a connecting intent once and keeps a failed restore sticky", async () => {
    await render({ available: true }); page("pagehide"); page("pageshow"); await flush();
    expect(posts()).toHaveLength(2);
    const socket = Socket.instances[1];
    act(() => socket.onerror?.()); await flush();
    page("pagehide"); page("pageshow"); await flush();
    await render({ available: true, uptimeMs: 10000 });
    expect(posts()).toHaveLength(2); expect(session.getState().phase).toBe("disconnected");
  });
});
