// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { J4SerialSession } from "./j4SerialSession";
import { createTestRunner, type RunnerCallbacks } from "./testRunner";
import type { RunSummary, TestScript } from "./testScript";

const wireState = (extra = {}) => ({ generation: 1, state: "ready", dtr: false,
  line_coding: { baud: 115200, data_bits: 8, parity: "none", stop_bits: 1, flow_control: "none" }, ...extra });
class Socket {
  static OPEN = 1; static instances: Socket[] = [];
  readyState = 0; bufferedAmount = 0; binaryType = "";
  onopen: (() => void) | null = null;
  onmessage: ((event: { data: unknown }) => void) | null = null;
  onclose: (() => void) | null = null; onerror = null;
  afterBinary: ((bytes: Uint8Array) => void) | null = null;
  sent: (string | Uint8Array)[] = [];
  constructor(public url: string) { Socket.instances.push(this); }
  send(data: string | Uint8Array) {
    this.sent.push(data);
    if (typeof data !== "string") queueMicrotask(() => this.afterBinary?.(data));
  }
  close = vi.fn(() => { this.readyState = 3; this.onclose?.(); });
  frame(data: object) { this.onmessage?.({ data: JSON.stringify({ topic: "serial.j4", ...data }) }); }
  bytes(bytes: Uint8Array) { this.onmessage?.({ data: new Uint8Array(bytes).buffer }); }
}
let owners: J4SerialSession[];
let fetchMock: ReturnType<typeof vi.fn>;
function owner() { const session = new J4SerialSession(); owners.push(session); return session; }
async function connect(session: J4SerialSession) {
  await session.connect(); const socket = Socket.instances.at(-1)!;
  socket.readyState = 1; socket.onopen?.(); socket.frame({ type: "serial_state", ...wireState() });
  return socket;
}
beforeEach(() => {
  owners = []; Socket.instances = []; let nextId = 1;
  vi.stubGlobal("WebSocket", Socket);
  fetchMock = vi.fn(async (url: string, init?: RequestInit) => new Response(JSON.stringify(
    url.endsWith("/serial/j4") ? wireState() : init?.method === "POST"
      ? { session_id: nextId, ws_url: `/api/v1/live-sessions/${nextId++}/ws` } : { ok: true })));
  vi.stubGlobal("fetch", fetchMock);
});
afterEach(async () => { await Promise.all(owners.map((session) => session.disconnect())); vi.unstubAllGlobals(); });

describe("shared J4 session owner", () => {
  it("fans out raw bytes and streaming UTF-8 to independent consumers on one MCU socket", async () => {
    const session = owner(); const socket = await connect(session);
    const raw: number[] = []; const text: string[] = [];
    const unsubscribeBytes = session.subscribeBytes((bytes) => raw.push(...bytes));
    const unsubscribeText = session.subscribe((chunk) => text.push(chunk), "j4");
    const bytes = new Uint8Array([0, 255, ...new TextEncoder().encode("登录")]);
    socket.bytes(bytes.slice(0, 3)); socket.bytes(bytes.slice(3));
    expect(raw).toEqual([...bytes]); expect(text.join("")).toBe("\0�登录");
    expect(session.connectedChannels()).toEqual(["j4"]);
    expect(Socket.instances).toHaveLength(1);
    unsubscribeBytes(); unsubscribeText(); socket.bytes(new Uint8Array([65]));
    expect(raw).toEqual([...bytes]); expect(text.join("")).toBe("\0�登录");
  });

  it("locks manual input while automation uses the same bounded backend, not a second connection", async () => {
    const session = owner(); const socket = await connect(session);
    let eventId = 0;
    socket.afterBinary = (bytes) => {
      socket.frame({ type: "serial_write_accepted", event_id: ++eventId, bytes: bytes.length });
      socket.frame({ type: "serial_write_complete", event_id: eventId, disposition: "completed" });
    };
    session.setAutomationActive(true, "j4");
    await expect(session.writeManual("manual")).rejects.toMatchObject({ code: "automation_active" });
    await expect(session.writeBinaryManual(new Uint8Array([255]))).rejects.toMatchObject({ code: "automation_active" });
    await session.write("中\0", "j4");
    expect([...(socket.sent[1] as Uint8Array)]).toEqual([0xe4, 0xb8, 0xad, 0]);
    session.setAutomationActive(false, "j4"); await session.writeManual("ok");
    expect(Socket.instances).toHaveLength(1);
    expect(fetchMock.mock.calls.filter(([, init]) => init?.method === "POST")).toHaveLength(1);
  });

  it("executes J4 send/wait/expect and condition branches over the real service protocol without terminal DOM", async () => {
    const session = owner(); const socket = await connect(session);
    const utf8 = new TextEncoder(); const decoder = new TextDecoder(); let eventId = 0;
    socket.afterBinary = (bytes) => {
      socket.frame({ type: "serial_write_accepted", event_id: ++eventId, bytes: bytes.length });
      socket.frame({ type: "serial_write_complete", event_id: eventId, disposition: "completed" });
      const text = decoder.decode(bytes);
      if (text === "boot\n") {
        const rx = new Uint8Array([0, 255, ...utf8.encode("登录: ")]);
        socket.bytes(rx.slice(0, 3)); socket.bytes(rx.slice(3));
      }
      const marker = /__LINKR_TASK_\d+__/.exec(text)?.[0];
      if (marker) {
        const rx = utf8.encode(`Linux 中文\n${marker}:0\n`);
        socket.bytes(rx.slice(0, 8)); socket.bytes(rx.slice(8));
      }
    };
    const script: TestScript = { schema: "linkr-test.v1", name: "J4 pipeline", version: "1.0", steps: [
      { id: "send", type: "serial_send", params: { channel: "j4", text: "boot\n" } },
      { id: "wait", type: "serial_wait", params: { channel: "j4", pattern: "登录", timeout_ms: 100 } },
      { id: "condition", type: "condition", params: {
        check: { id: "check", type: "serial_expect", params: { channel: "j4", command: "uname -s", pattern: "Linux 中文", timeout_ms: 100 } },
        then_steps: [{ id: "chosen", type: "serial_send", params: { channel: "j4", text: "chosen\n" } }],
        else_steps: [{ id: "unused", type: "serial_send", params: { channel: "j4", text: "unused\n" } }],
      } },
    ] };
    let summary: RunSummary | null = null; const channels: string[] = [];
    const callbacks: RunnerCallbacks = {
      onStepStart() { expect(session.getState().automationActive).toBe(true); }, onStepResult() {}, onAdcSample() {}, onPowerCapture() {},
      onSerialLog: (_id, channel) => { channels.push(channel); },
      onComplete: (result) => { summary = result; }, onError: (error) => { throw new Error(error); },
    };
    await createTestRunner(script, { current: { connected: true } } as never, { current: session }, callbacks).start();
    const result = summary as RunSummary | null;
    expect(result?.completed).toBe(true); expect(result?.failed).toBe(0); expect(result?.errored).toBe(0);
    expect(result?.results.map((step) => step.status)).toEqual(["pass", "pass", "pass", "pass", "skip"]);
    expect(channels.every((channel) => channel === "j4")).toBe(true);
    expect(socket.sent.filter((item) => typeof item !== "string").map((item) => decoder.decode(item as Uint8Array))).not.toContain("unused\n");
    expect(Socket.instances).toHaveLength(1); expect(document.querySelector('[role="application"]')).toBeNull();
    expect(session.getState().automationActive).toBe(false);
  });

  it("resets automation's UTF-8 stream at generation boundaries and isolates separate owners", async () => {
    const a = owner(); const b = owner(); const first = await connect(a); const second = await connect(b);
    const text: string[] = []; a.subscribe((chunk) => text.push(chunk));
    first.bytes(new Uint8Array([0xe4])); first.frame({ type: "serial_state", ...wireState({ generation: 2 }) });
    first.bytes(new Uint8Array([0xb8, 0xad]));
    expect(text.join("")).not.toBe("中");
    await a.disconnect(); expect(second.close).not.toHaveBeenCalled(); expect(b.isConnected("j4")).toBe(true);
  });
});
