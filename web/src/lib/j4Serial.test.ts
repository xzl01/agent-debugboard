// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { J4SerialClient, j4SerialTxTimeoutMs, J4_SERIAL_TX_MIN_TIMEOUT_MS, J4_SERIAL_TX_SCHEDULING_GUARD_MS } from "./j4Serial";
import { J4_SERIAL_BUFFER_LIMIT, J4_SERIAL_FRAME_BYTES, J4_SERIAL_QUEUE_LIMIT, type J4SerialConfig } from "./j4SerialProtocol";

const config = { baud: 115200, data_bits: 8, parity: "none", stop_bits: 1, flow_control: "none" } as const;
const state = (extra = {}) => ({ generation: 1, state: "ready", line_coding: config, dtr: false, read_only: false, ...extra });
const response = (data: unknown, status = 200) => new Response(JSON.stringify(data), { status });

class Socket {
  static OPEN = 1;
  static instances: Socket[] = [];
  readyState = 0;
  binaryType = "blob";
  bufferedAmount = 0;
  sent: (string | Uint8Array)[] = [];
  onopen: (() => void) | null = null;
  onmessage: ((event: { data: unknown }) => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  constructor(public url: string) { Socket.instances.push(this); }
  send(data: string | Uint8Array) { this.sent.push(typeof data === "string" ? data : data.slice()); }
  close = vi.fn(() => { this.readyState = 3; this.onclose?.(); });
  open() { this.readyState = 1; this.onopen?.(); }
  frame(frame: unknown) { this.onmessage?.({ data: JSON.stringify(frame) }); }
  bytes(bytes: number[]) { this.onmessage?.({ data: new Uint8Array(bytes).buffer }); }
}

let clients: J4SerialClient[];
let nextId: number;
let fetchMock: ReturnType<typeof vi.fn>;
function client() {
  const onChange = vi.fn(); const onBytes = vi.fn();
  const serial = new J4SerialClient(onChange, onBytes);
  clients.push(serial);
  return { serial, onChange, onBytes };
}
async function connected(serial: J4SerialClient) {
  await serial.connect();
  const socket = Socket.instances.at(-1)!;
  socket.open();
  socket.frame({ type: "serial_state", topic: "serial.j4", ...state() });
  return socket;
}
function complete(socket: Socket, id: number, bytes = 1, disposition = "completed") {
  socket.frame({ type: "serial_write_accepted", topic: "serial.j4", event_id: id, bytes });
  socket.frame({ type: "serial_write_complete", topic: "serial.j4", event_id: id, disposition });
}

beforeEach(() => {
  vi.useFakeTimers(); clients = []; nextId = 10; Socket.instances = [];
  vi.stubGlobal("WebSocket", Socket);
  fetchMock = vi.fn(async (url: string, init?: RequestInit) => {
    if (url.endsWith("/serial/j4")) return response(state());
    if (url.endsWith("/live-sessions") && init?.method === "POST") {
      const id = nextId++;
      return response({ ok: true, session_id: id, ws_url: `ws://172.29.203.1/api/v1/live-sessions/${id}/ws`, connected: false }, 201);
    }
    if (init?.method === "DELETE") return response({ ok: true });
    throw new Error(`Unexpected request ${init?.method} ${url}`);
  });
  vi.stubGlobal("fetch", fetchMock);
});
afterEach(async () => {
  await Promise.all(clients.map((serial) => serial.disconnect()));
  vi.useRealTimers(); vi.unstubAllGlobals();
});

describe("J4 MCU live-session transport", () => {
  it.each([
    [{ ...config, baud: 4800, parity: "even", stop_bits: 2 }, 11600],
    [{ ...config, baud: 5200 }, 9385],
    [{ ...config, baud: 4800, data_bits: 5 }, 7600],
    [{ ...config, baud: 4800, stop_bits: 2 }, 10800],
    [config, 5000],
  ] satisfies [J4SerialConfig, number][])("calculates a bounded shared-queue TX timeout for %j", (current, expected) => {
    expect(j4SerialTxTimeoutMs(current)).toBe(expected);
    expect(J4_SERIAL_TX_MIN_TIMEOUT_MS).toBe(5000);
    expect(J4_SERIAL_TX_SCHEDULING_GUARD_MS).toBe(2000);
  });

  it("uses the conservative minimum-baud worst framing when current config is missing or invalid", () => {
    for (const current of [undefined, null, { ...config, baud: 0 }, { ...config, data_bits: 9 }]) {
      expect(j4SerialTxTimeoutMs(current as J4SerialConfig | null | undefined)).toBe(11600);
    }
  });

  it.each([
    { ...config, baud: 4800, parity: "even", stop_bits: 2 },
    { ...config, baud: 5200 },
  ] satisfies J4SerialConfig[])("allows seven competing firmware slots beyond 5 s using authoritative send-time config %j", async (current) => {
    const { serial } = client(); const socket = await connected(serial);
    // The connect GET reported 115200. The later MCU state is the send authority.
    socket.frame({ type: "serial_state", ...state({ generation: 2, line_coding: current }) });
    const write = serial.writeBytes(new Uint8Array(J4_SERIAL_FRAME_BYTES));
    const outcome = write.then(() => "complete", (reason: unknown) => reason);
    const bits = 1 + current.data_bits + (current.parity !== "none" ? 1 : 0) + current.stop_bits;
    const competingMs = Math.ceil(J4_SERIAL_FRAME_BYTES * (J4_SERIAL_QUEUE_LIMIT - 1) * bits * 1000 / current.baud);
    const ownWireMs = Math.ceil(J4_SERIAL_FRAME_BYTES * bits * 1000 / current.baud);
    socket.frame({ type: "serial_write_accepted", topic: "serial.j4", event_id: 1, bytes: J4_SERIAL_FRAME_BYTES });
    await vi.advanceTimersByTimeAsync(5001);
    expect(serial.getSnapshot().phase).toBe("connected");
    expect(serial.getSnapshot().pendingWrites).toBe(1);
    expect(socket.sent).toHaveLength(2); // Subscription and exactly one TX frame.
    await vi.advanceTimersByTimeAsync(competingMs - 5001 + ownWireMs);
    expect(competingMs + ownWireMs).toBeLessThan(j4SerialTxTimeoutMs(current));
    expect(socket.close).not.toHaveBeenCalled();
    socket.frame({ type: "serial_write_complete", topic: "serial.j4", event_id: 1, disposition: "completed" });
    expect(await outcome).toBe("complete");
    expect(serial.getSnapshot().txBytes).toBe(J4_SERIAL_FRAME_BYTES);
    expect(serial.getSnapshot().pendingWrites).toBe(0);
    expect(serial.getSnapshot().phase).toBe("connected");
  });

  it.each([
    [{ ...config, baud: 4800, parity: "even", stop_bits: 2 }, 11600],
    [{ ...config, baud: 5200 }, 9385],
    [config, 5000],
  ] satisfies [J4SerialConfig, number][])("fails at the exact bounded TX deadline for %j without extending on acceptance", async (current, timeoutMs) => {
    const { serial } = client(); const socket = await connected(serial);
    socket.frame({ type: "serial_state", ...state({ generation: 2, line_coding: current }) });
    const write = serial.writeBytes(new Uint8Array(J4_SERIAL_FRAME_BYTES));
    const rejected = expect(write).rejects.toMatchObject({ code: "disconnected" });
    await vi.advanceTimersByTimeAsync(timeoutMs - 100);
    socket.frame({ type: "serial_write_accepted", topic: "serial.j4", event_id: 1, bytes: J4_SERIAL_FRAME_BYTES });
    await vi.advanceTimersByTimeAsync(99);
    expect(serial.getSnapshot().phase).toBe("connected");
    expect(socket.close).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(1);
    await rejected;
    expect(serial.getSnapshot().phase).toBe("disconnected");
    expect(serial.getSnapshot().error?.code).toBe("tx_timeout");
    expect(socket.close).toHaveBeenCalledTimes(1);
    expect(serial.getSnapshot().pendingWrites).toBe(0);
    const sessionPosts = fetchMock.mock.calls.filter(([, init]) => init?.method === "POST").length;
    await vi.advanceTimersByTimeAsync(60_000);
    expect(fetchMock.mock.calls.filter(([, init]) => init?.method === "POST")).toHaveLength(sessionPosts);
  });

  it("accepts an initial GET and serial_state with arbitrary 5200 baud", async () => {
    const original = fetchMock.getMockImplementation()!;
    const reported = state({ line_coding: { ...config, baud: 5200 } });
    fetchMock.mockImplementation((url: string, init?: RequestInit) => url.endsWith("/serial/j4") ? Promise.resolve(response(reported)) : original(url, init));
    const { serial } = client();
    await serial.connect();
    expect(serial.getSnapshot().serial?.config.baud).toBe(5200);
    const socket = Socket.instances.at(-1)!;
    socket.open(); socket.frame({ type: "serial_state", ...reported });
    expect(serial.getSnapshot().phase).toBe("connected");
    expect(serial.getSnapshot().serial?.config.baud).toBe(5200);
  });

  it.each([4799, 115201, 5200.5, NaN, Infinity])("rejects configure baud %s before any PUT or config-state change", async (baud) => {
    const { serial } = client(); await connected(serial);
    await expect(serial.configure({ ...config, baud })).rejects.toMatchObject({ code: "invalid_baud" });
    expect(fetchMock.mock.calls.some(([, init]) => init?.method === "PUT")).toBe(false);
    expect(serial.getSnapshot().configuring).toBe(false);
    expect(serial.getSnapshot().serial?.config.baud).toBe(115200);
  });

  it("uses only board REST and a rewritten same-origin dedicated WS", async () => {
    const browserSerial = { getPorts: vi.fn(), requestPort: vi.fn() };
    Object.defineProperty(navigator, "serial", { configurable: true, value: browserSerial });
    const { serial } = client();
    const socket = await connected(serial);
    expect(fetchMock.mock.calls.map(([url]) => url)).toEqual(["/api/v1/serial/j4", "/api/v1/live-sessions"]);
    expect(socket.url).toBe(`ws://${location.host}/api/v1/live-sessions/10/ws`);
    expect(socket.url).not.toContain("8787");
    expect(socket.binaryType).toBe("arraybuffer");
    expect(socket.sent).toEqual([JSON.stringify({ type: "subscribe", topic: "serial.j4" })]);
    expect(browserSerial.getPorts).not.toHaveBeenCalled(); expect(browserSerial.requestPort).not.toHaveBeenCalled();
    expect(serial.getSnapshot().phase).toBe("connected");
  });

  it("delivers arbitrary binary RX, including NUL/FF and split UTF-8, unchanged", async () => {
    const { serial, onBytes } = client(); const socket = await connected(serial);
    socket.bytes([]);
    socket.bytes([0, 255, 0xe4]); socket.bytes([0xb8, 0xad, 13]);
    expect(onBytes.mock.calls.map(([bytes]) => [...bytes])).toEqual([[0, 255, 0xe4], [0xb8, 0xad, 13]]);
    expect(serial.getSnapshot().rxBytes).toBe(6);
  });

  it("sends atomic binary TX chunks of at most 480 bytes and waits for physical completion", async () => {
    const { serial } = client(); const socket = await connected(serial);
    const bytes = new Uint8Array(1001).map((_, index) => index & 255);
    let resolved = false;
    const write = serial.writeBytes(bytes).then(() => { resolved = true; });
    expect(socket.sent).toHaveLength(2); // Subscribe + one MCU in-flight slot.
    socket.frame({ type: "serial_write_complete", disposition: "completed" });
    expect(serial.getSnapshot().pendingWrites).toBe(3);
    expect(serial.getSnapshot().txBytes).toBe(0);
    socket.frame({ type: "serial_write_accepted", event_id: 1, bytes: 480 });
    await Promise.resolve(); expect(resolved).toBe(false); expect(serial.getSnapshot().txBytes).toBe(0);
    socket.frame({ type: "serial_write_complete", event_id: 1, disposition: "completed" });
    expect(socket.sent).toHaveLength(3);
    complete(socket, 2, 480); complete(socket, 3, 41); await write;
    const frames = socket.sent.filter((item): item is Uint8Array => item instanceof Uint8Array);
    expect(frames.map((item) => item.length)).toEqual([480, 480, 41]);
    expect([...frames[0]].slice(0, 2)).toEqual([0, 1]); expect(frames[0][255]).toBe(255);
    expect([...frames.flatMap((frame) => [...frame])]).toEqual([...bytes]);
    expect(serial.getSnapshot().txBytes).toBe(1001);
  });

  it("encodes xterm text as UTF-8, not JSON or a binary-string conversion", async () => {
    const { serial } = client(); const socket = await connected(serial);
    const write = serial.writeText("\0中\u00ff");
    expect([...(socket.sent[1] as Uint8Array)]).toEqual([0, 0xe4, 0xb8, 0xad, 0xc3, 0xbf]);
    complete(socket, 1, 6); await write;
  });

  it("bounds the queue at eight, rejects an oversize paste wholly and exposes bufferedAmount pressure", async () => {
    const { serial } = client(); const socket = await connected(serial);
    await expect(serial.writeBytes(new Uint8Array(J4_SERIAL_BUFFER_LIMIT + 1))).rejects.toMatchObject({ code: "backpressure" });
    expect(socket.sent).toHaveLength(1);
    const writes = Array.from({ length: 8 }, () => serial.writeBytes(new Uint8Array(J4_SERIAL_FRAME_BYTES)));
    expect(serial.getSnapshot().pendingWrites).toBe(8);
    await expect(serial.writeText("ninth")).rejects.toMatchObject({ code: "backpressure" });
    expect(socket.sent).toHaveLength(2);
    for (let id = 1; id <= 8; id++) complete(socket, id, 480);
    await Promise.all(writes);
    expect(socket.sent).toHaveLength(9);
    socket.bufferedAmount = J4_SERIAL_BUFFER_LIMIT;
    await expect(serial.writeText("blocked")).rejects.toMatchObject({ code: "backpressure" });
    expect(serial.getSnapshot().backpressure).toBe(true);
    socket.bufferedAmount = 0; await vi.advanceTimersByTimeAsync(100);
    expect(serial.getSnapshot().backpressure).toBe(false);
  });

  it("cleans up only its own session and never replays writes on manual reconnect", async () => {
    const { serial } = client(); const socket = await connected(serial);
    const staleMessage = socket.onmessage!;
    const write = serial.writeText("not replayed");
    const rejection = expect(write).rejects.toMatchObject({ code: "disconnected" });
    await serial.disconnect(); await rejection;
    expect(socket.close).toHaveBeenCalledOnce();
    expect(fetchMock).toHaveBeenCalledWith("/api/v1/live-sessions/10", expect.objectContaining({ method: "DELETE" }));
    const next = await connected(serial);
    staleMessage({ data: JSON.stringify({ type: "serial_state", ...state({ generation: 99, state: "faulted" }) }) });
    staleMessage({ data: new Uint8Array([255]).buffer });
    expect(serial.getSnapshot().serial?.generation).toBe(1);
    expect(serial.getSnapshot().rxBytes).toBe(0);
    expect(next.sent).toHaveLength(1);
  });

  it("keeps two component instances independent", async () => {
    const a = client(); const b = client();
    const first = await connected(a.serial); const second = await connected(b.serial);
    first.bytes([1]); second.bytes([2, 3]);
    await a.serial.disconnect();
    expect(second.close).not.toHaveBeenCalled(); expect(b.serial.getSnapshot().phase).toBe("connected");
    expect(a.onBytes.mock.calls[0][0]).toEqual(new Uint8Array([1]));
    expect(b.onBytes.mock.calls[0][0]).toEqual(new Uint8Array([2, 3]));
    expect(fetchMock.mock.calls.filter(([url, init]) => init?.method === "DELETE").map(([url]) => url)).toEqual(["/api/v1/live-sessions/10"]);
  });

  it("keeps a write rejection nonfatal without imposing any TX ownership or read-only lock", async () => {
    const { serial, onBytes } = client(); const socket = await connected(serial);
    const denied = expect(serial.writeBytes(new Uint8Array(J4_SERIAL_FRAME_BYTES * 3))).rejects.toMatchObject({ code: "write_rejected" });
    expect(socket.sent).toHaveLength(2); // One in-flight chunk, two local chunks.
    socket.frame({ type: "error", topic: "serial.j4", error: { code: "write_rejected", message: "not admitted" } });
    await denied;
    expect(serial.getSnapshot()).toMatchObject({ phase: "connected", serial: { read_only: false }, error: { code: "write_rejected" }, pendingWrites: 0 });
    expect(socket.close).not.toHaveBeenCalled();
    socket.bytes([27, 91, 54, 110, 65]);
    expect(onBytes).toHaveBeenCalledWith(new Uint8Array([27, 91, 54, 110, 65]));
    expect(serial.getSnapshot().rxBytes).toBe(5);
    await vi.advanceTimersByTimeAsync(60_000);
    expect(socket.close).not.toHaveBeenCalled();
    expect(fetchMock.mock.calls.filter(([, init]) => init?.method === "POST")).toHaveLength(1);
    // Explicit new input remains allowed; discarded old input is never replayed.
    const write = serial.writeText("new"); complete(socket, 2, 3); await write;
    expect(socket.sent).toHaveLength(3); // Nothing from the discarded queue.
  });

  it.each(["completed", "canceled"])("preserves accepted event completion %s while responder changes and manual writers remain enabled", async (disposition) => {
    const { serial } = client(); const socket = await connected(serial);
    const accepted = serial.writeText("accepted");
    const acceptedResult = accepted.then(() => "complete", (error: { code: string }) => error.code);
    let settled = false; void acceptedResult.then(() => { settled = true; });
    socket.frame({ type: "serial_write_accepted", event_id: 44, bytes: 8 });
    const localResult = expect(serial.writeText("local")).rejects.toMatchObject({ code: "write_rejected" });
    socket.frame({ type: "error", code: "write_rejected", message: "queue rejected" });
    await localResult;
    expect(settled).toBe(false);
    expect(serial.getSnapshot().pendingWrites).toBe(1);
    socket.frame({ type: "serial_state", ...state({ terminal_responder: false, dtr: true, read_only: false }) });
    expect(serial.getSnapshot().serial?.read_only).toBe(false);
    expect(serial.getSnapshot().pendingWrites).toBe(1);
    expect(socket.close).not.toHaveBeenCalled();
    socket.frame({ type: "serial_write_complete", event_id: 44, disposition });
    expect(await acceptedResult).toBe(disposition === "completed" ? "complete" : "tx_canceled");
    expect(serial.getSnapshot().txBytes).toBe(disposition === "completed" ? 8 : 0);
    expect(serial.getSnapshot().pendingWrites).toBe(0);
    expect(serial.getSnapshot().phase).toBe("connected");
    expect(socket.sent).toHaveLength(2);
  });

  it("does not cancel sent or local manual writes when DTR selects another automatic responder", async () => {
    const { serial } = client(); const socket = await connected(serial);
    const inFlight = serial.writeText("A");
    const local = serial.writeText("B");
    socket.frame({ type: "serial_state", ...state({ terminal_responder: { transport: "raw_tcp", slot: 2 }, dtr: true, read_only: false }) });
    expect(socket.close).not.toHaveBeenCalled();
    expect(serial.getSnapshot().pendingWrites).toBe(2);
    complete(socket, 20); await inFlight;
    complete(socket, 21); await local;
    expect(serial.getSnapshot().phase).toBe("connected");
    expect(socket.sent).toHaveLength(3);
  });

  it("models two explicit Web writers with only one automatic terminal responder and no TX-owner acquisition", async () => {
    const first = client(); const second = client();
    const a = await connected(first.serial); const b = await connected(second.serial);
    const reportResponder = (firstResponder: boolean, secondResponder: boolean) => {
      a.frame({ type: "serial_state", ...state({ terminal_responder: firstResponder, read_only: false }) });
      b.frame({ type: "serial_state", ...state({ terminal_responder: secondResponder, read_only: false }) });
    };
    reportResponder(true, false);
    const firstManual = first.serial.writeText("A");
    const secondManual = second.serial.writeText("B");
    complete(a, 1); complete(b, 2); await Promise.all([firstManual, secondManual]);
    a.bytes([65]); b.bytes([65]);
    expect(second.onBytes).toHaveBeenCalledWith(new Uint8Array([65]));
    expect(second.serial.getSnapshot().phase).toBe("connected");
    expect(first.serial.getSnapshot().serial?.read_only).toBe(false);
    expect(second.serial.getSnapshot().serial?.read_only).toBe(false);
    expect(first.serial.getSnapshot().serial?.terminal_responder).toBe(true);
    expect(second.serial.getSnapshot().serial?.terminal_responder).toBe(false);
    await vi.advanceTimersByTimeAsync(20_000);
    expect(fetchMock.mock.calls.filter(([, init]) => init?.method === "POST")).toHaveLength(2);
    reportResponder(false, true);
    const next = second.serial.writeText("C"); complete(b, 3); await next;
    expect(second.serial.getSnapshot().error).toBeNull();
    expect(a.close).not.toHaveBeenCalled(); expect(b.close).not.toHaveBeenCalled();
  });

  it("preserves confirmed per-client WS responder authorization across a corroborating global config GET", async () => {
    const { serial } = client(); const socket = await connected(serial);
    const responder = { transport: "websocket", slot: 0 };
    socket.frame({ type: "serial_state", ...state({ terminal_responder: true, responder }) });
    const original = fetchMock.getMockImplementation()!;
    let result = state({ generation: 2, terminal_responder: responder,
      line_coding: { ...config, baud: 5200 }, config_request: { id: 7, done: true, result: 0 } });
    fetchMock.mockImplementation((url: string, init?: RequestInit) => init?.method === "PUT"
      ? Promise.resolve(response({ accepted: true, request_id: 7 }, 202))
      : url.endsWith("/serial/j4") ? Promise.resolve(response(result)) : original(url, init));
    await serial.configure({ ...config, baud: 5200 });
    await vi.advanceTimersByTimeAsync(250);
    expect(serial.getSnapshot().serial).toMatchObject({ terminal_responder: true, responder, config: { baud: 5200 } });
    expect(serial.getSnapshot().configuring).toBe(false);
    // A different global responder must not authorize this page by inference.
    result = state({ generation: 3, terminal_responder: { transport: "websocket", slot: 1 },
      line_coding: { ...config, baud: 9600 }, config_request: { id: 7, done: true, result: 0 } });
    await serial.configure({ ...config, baud: 9600 });
    await vi.advanceTimersByTimeAsync(250);
    expect(serial.getSnapshot().serial).toMatchObject({ terminal_responder: false, read_only: false, responder: { transport: "websocket", slot: 1 } });
  });

  it("shows unavailable once with no session allocation or reconnect storm on old firmware", async () => {
    fetchMock.mockImplementation(async () => response({ ok: false, error: { code: "not_found" } }, 404));
    const { serial } = client(); await serial.connect();
    expect(serial.getSnapshot().phase).toBe("unavailable");
    await vi.advanceTimersByTimeAsync(60_000);
    expect(fetchMock).toHaveBeenCalledOnce(); expect(Socket.instances).toHaveLength(0);
  });

  it("handles a supported status endpoint with unavailable hardware without opening a session", async () => {
    fetchMock.mockResolvedValue(response({ ok: true, available: false, port: "j4", state: "unavailable" }));
    const { serial } = client(); await serial.connect();
    expect(serial.getSnapshot().phase).toBe("unavailable");
    expect(Socket.instances).toHaveLength(0);
  });

  it("discards a late session-creation response after cancellation", async () => {
    const original = fetchMock.getMockImplementation()!;
    let resolve!: (value: Response) => void;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => url.endsWith("/live-sessions")
      ? new Promise<Response>((done) => { resolve = done; }) : original(url, init));
    const { serial } = client(); const connect = serial.connect();
    await vi.waitFor(() => expect(resolve).toBeTypeOf("function"));
    await serial.disconnect();
    resolve(response({ session_id: 77, ws_url: "/api/v1/live-sessions/77/ws" })); await connect;
    expect(Socket.instances).toHaveLength(0);
    expect(fetchMock).toHaveBeenCalledWith("/api/v1/live-sessions/77", expect.objectContaining({ method: "DELETE" }));
  });

  it("accounts for drops and blocks paused, faulted and read-only states", async () => {
    const { serial } = client(); const socket = await connected(serial);
    socket.frame({ type: "serial_overrun", topic: "serial.j4", dropped_bytes: 12, total_dropped_bytes: 12 });
    socket.frame({ type: "serial_overrun", topic: "serial.j4", dropped_bytes: 12, total_dropped_bytes: 24 });
    expect(serial.getSnapshot().droppedBytes).toBe(24);
    socket.frame({ type: "serial_overrun", topic: "serial.j4", dropped_bytes: 12, total_dropped_bytes: 24 });
    socket.frame({ type: "serial_overrun", topic: "serial.j4", dropped_bytes: 12, total_dropped_bytes: 12 });
    expect(serial.getSnapshot().droppedBytes).toBe(24);
    for (const change of [{ state: "paused" }, { state: "faulted" }, { read_only: true }]) {
      socket.frame({ type: "serial_state", ...state(change) });
      await expect(serial.writeText("x")).rejects.toMatchObject({ code: "read_only" });
    }
    socket.frame({ type: "error", command: "subscribe", error: { code: "serial_unavailable", message: "not available" } });
    expect(serial.getSnapshot().phase).toBe("unavailable");
  });

  it("resets the overrun baseline on reconnect without importing a reused slot's historical total", async () => {
    const { serial } = client(); const first = await connected(serial);
    first.frame({ type: "serial_overrun", topic: "serial.j4", dropped_bytes: 12, total_dropped_bytes: 12 });
    first.frame({ type: "serial_overrun", topic: "serial.j4", dropped_bytes: 12, total_dropped_bytes: 24 });
    first.frame({ type: "serial_state", ...state({ generation: 2 }) });
    first.frame({ type: "serial_overrun", topic: "serial.j4", dropped_bytes: 12, total_dropped_bytes: 36 });
    expect(serial.getSnapshot().droppedBytes).toBe(36);
    const staleMessage = first.onmessage!;
    const next = await connected(serial);
    staleMessage({ data: JSON.stringify({ type: "serial_overrun", topic: "serial.j4", dropped_bytes: 12, total_dropped_bytes: 48 }) });
    next.frame({ type: "serial_overrun", topic: "serial.j4", dropped_bytes: 2, total_dropped_bytes: 2 });
    expect(serial.getSnapshot().droppedBytes).toBe(38);
    const reused = await connected(serial);
    reused.frame({ type: "serial_overrun", topic: "serial.j4", dropped_bytes: 3, total_dropped_bytes: 103 });
    reused.frame({ type: "serial_overrun", topic: "serial.j4", dropped_bytes: 3, total_dropped_bytes: 106 });
    expect(serial.getSnapshot().droppedBytes).toBe(44);
  });

  it("cancels pending writes at generation boundaries and ignores stale completions", async () => {
    const { serial } = client(); const socket = await connected(serial);
    const write = serial.writeText("old"); const rejection = expect(write).rejects.toMatchObject({ code: "generation_changed" });
    socket.frame({ type: "serial_write_accepted", event_id: 1, bytes: 3 });
    socket.frame({ type: "serial_state", ...state({ generation: 2 }) }); await rejection;
    const next = serial.writeText("new");
    socket.frame({ type: "serial_write_complete", event_id: 1, disposition: "completed" });
    expect(serial.getSnapshot().pendingWrites).toBe(1);
    complete(socket, 2, 3); await next;
    expect(serial.getSnapshot().txBytes).toBe(3);
  });

  it("closes an ambiguous unacknowledged generation boundary instead of reassigning an old acceptance", async () => {
    const { serial } = client(); const socket = await connected(serial);
    const write = serial.writeText("old"); const rejection = expect(write).rejects.toMatchObject({ code: "disconnected" });
    socket.frame({ type: "serial_state", ...state({ generation: 2 }) }); await rejection;
    expect(serial.getSnapshot().phase).toBe("disconnected");
    expect(serial.getSnapshot().error?.code).toBe("generation_changed");
    expect(socket.close).toHaveBeenCalledOnce();
  });

  it("does not send queued chunks after a canceled active frame", async () => {
    const { serial } = client(); const socket = await connected(serial);
    const write = serial.writeBytes(new Uint8Array(960));
    const rejection = expect(write).rejects.toMatchObject({ code: "tx_canceled" });
    complete(socket, 1, 480, "canceled"); await rejection;
    expect(socket.sent).toHaveLength(2);
    expect(serial.getSnapshot().pendingWrites).toBe(0);
    expect(serial.getSnapshot().error?.code).toBe("tx_canceled");
  });

  it("uses expected_generation and leaves 202 config admission pending until completion", async () => {
    const { serial } = client(); const socket = await connected(serial);
    const original = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => init?.method === "PUT"
      ? Promise.resolve(response({ ok: true, accepted: true, request_id: 5 }, 202)) : original(url, init));
    await serial.configure({ ...config, baud: 57600 });
    expect(fetchMock).toHaveBeenCalledWith("/api/v1/serial/j4/config", expect.objectContaining({ method: "PUT",
      body: JSON.stringify({ ...config, baud: 57600, expected_generation: 1 }) }));
    expect(serial.getSnapshot().configuring).toBe(true);
    await expect(serial.writeText("wait")).rejects.toMatchObject({ code: "read_only" });
    fetchMock.mockImplementation((url: string, init?: RequestInit) => url.endsWith("/serial/j4")
      ? Promise.resolve(response(state({ generation: 2, line_coding: { ...config, baud: 57600 }, config_request: { id: 5, done: true, result: 0 } }))) : original(url, init));
    socket.frame({ type: "serial_state", ...state({ generation: 2, line_coding: { ...config, baud: 57600 } }) });
    expect(serial.getSnapshot().configuring).toBe(true);
    await vi.advanceTimersByTimeAsync(250);
    expect(serial.getSnapshot().configuring).toBe(false);
    expect(serial.getSnapshot().serial?.config.baud).toBe(57600);
  });

  it("surfaces DTR/CAS conflicts and refreshes actual settings without retrying PUT", async () => {
    const { serial } = client(); await connected(serial);
    const original = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => init?.method === "PUT"
      ? Promise.resolve(response({ ok: false, error: { code: "config_conflict", message: "DTR active" } }, 409))
      : url.endsWith("/serial/j4") ? Promise.resolve(response(state({ dtr: true, generation: 2, line_coding: { ...config, baud: 5200 } }))) : original(url, init));
    await expect(serial.configure({ ...config, baud: 9600 })).rejects.toMatchObject({ code: "config_conflict" });
    expect(serial.getSnapshot().error?.code).toBe("config_conflict");
    expect(serial.getSnapshot().serial?.dtr).toBe(true);
    expect(serial.getSnapshot().serial?.generation).toBe(2);
    expect(serial.getSnapshot().serial?.config.baud).toBe(5200);
    expect(fetchMock.mock.calls.filter(([, init]) => init?.method === "PUT")).toHaveLength(1);
  });

  it("ignores an older completed config request and reports an async DTR conflict", async () => {
    const { serial } = client(); await connected(serial);
    const original = fetchMock.getMockImplementation()!;
    let result = state({ config_request: { id: 4, done: true, result: 0 } });
    fetchMock.mockImplementation((url: string, init?: RequestInit) => init?.method === "PUT"
      ? Promise.resolve(response({ accepted: true, request_id: 5 }, 202))
      : url.endsWith("/serial/j4") ? Promise.resolve(response(result)) : original(url, init));
    await serial.configure({ ...config, baud: 9600 });
    await vi.advanceTimersByTimeAsync(250);
    expect(serial.getSnapshot().configuring).toBe(true);
    result = state({ dtr: true, config_request: { id: 5, done: true, result: -16 } });
    await vi.advanceTimersByTimeAsync(250);
    expect(serial.getSnapshot().configuring).toBe(false);
    expect(serial.getSnapshot().error?.code).toBe("config_conflict");
  });

  it("bounds config-completion polling and closes without reconnecting on timeout", async () => {
    const { serial } = client(); await connected(serial);
    const original = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => init?.method === "PUT"
      ? Promise.resolve(response({ accepted: true, request_id: 5 }, 202)) : original(url, init));
    await serial.configure({ ...config, baud: 9600 });
    await vi.advanceTimersByTimeAsync(6000);
    expect(serial.getSnapshot().error?.code).toBe("config_timeout");
    expect(serial.getSnapshot().phase).toBe("disconnected");
    const count = fetchMock.mock.calls.length;
    await vi.advanceTimersByTimeAsync(60_000);
    expect(fetchMock).toHaveBeenCalledTimes(count);
    expect(fetchMock.mock.calls.filter(([url]) => url.endsWith("/serial/j4"))).toHaveLength(21);
  });

  it("waits for its WS close handshake before deleting a connected session", async () => {
    const { serial } = client(); const socket = await connected(serial);
    socket.close.mockImplementation(() => {});
    const disconnect = serial.disconnect();
    expect(fetchMock.mock.calls.some(([, init]) => init?.method === "DELETE")).toBe(false);
    socket.onclose?.(); await disconnect;
    expect(fetchMock.mock.calls.filter(([, init]) => init?.method === "DELETE")).toHaveLength(1);
  });
});
