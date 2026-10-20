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
    expect(posts()).toHaveLength(1);
    expect(session.getState().phase).toBe("disconnected");
  });

  it("reconnects once after a reboot epoch, clears old TX and deletes both independent sessions", async () => {
    await render({ available: true, uptimeMs: 10000 });
    const old = Socket.instances[0]; act(() => old.open());
    const write = session.writeManual("old TX");
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
    expect(deletions).toEqual(["/api/v1/live-sessions/1", "/api/v1/live-sessions/2"]);
  });

  it("starts only once under StrictMode and once more after an explicit reboot recovery", async () => {
    await render({ available: true, uptimeMs: 10000 }, true);
    expect(posts()).toHaveLength(1);
    await render({ available: true, uptimeMs: 12000, restarting: true }, true);
    await render({ available: true, uptimeMs: 100, restarting: false }, true);
    expect(posts()).toHaveLength(2);
  });
});
