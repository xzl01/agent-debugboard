import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { vi } from "vitest";
import { useBoard, type UseBoard } from "./useBoard";

const hoistedApiMocks = vi.hoisted(() => ({
  createLiveSession: vi.fn<typeof import("@/lib/api").createLiveSession>(),
  deleteLiveSession: vi.fn<typeof import("@/lib/api").deleteLiveSession>(),
  enterBootloader: vi.fn<typeof import("@/lib/api").enterBootloader>(),
  getAdc: vi.fn<typeof import("@/lib/api").getAdc>(),
  getStatus: vi.fn<typeof import("@/lib/api").getStatus>(),
  liveWebSocketUrl: vi.fn<typeof import("@/lib/api").liveWebSocketUrl>(),
  setGpio: vi.fn<typeof import("@/lib/api").setGpio>(),
  setPower: vi.fn<typeof import("@/lib/api").setPower>(),
  setSwitch: vi.fn<typeof import("@/lib/api").setSwitch>(),
}));

const hoistedPowerCaptureStoreMocks = vi.hoisted(() => ({
  appendPowerCaptureChunk:
    vi.fn<typeof import("@/lib/powerCaptureStore").appendPowerCaptureChunk>(),
  beginPowerCaptureArchive:
    vi.fn<typeof import("@/lib/powerCaptureStore").beginPowerCaptureArchive>(),
  clearPowerCaptureArchives:
    vi.fn<typeof import("@/lib/powerCaptureStore").clearPowerCaptureArchives>(),
  deletePowerCaptureArchive:
    vi.fn<typeof import("@/lib/powerCaptureStore").deletePowerCaptureArchive>(),
  ensurePowerCaptureStorageCapacity:
    vi.fn<typeof import("@/lib/powerCaptureStore").ensurePowerCaptureStorageCapacity>(),
  finishPowerCaptureArchive:
    vi.fn<typeof import("@/lib/powerCaptureStore").finishPowerCaptureArchive>(),
  interruptPowerCaptureArchive:
    vi.fn<typeof import("@/lib/powerCaptureStore").interruptPowerCaptureArchive>(),
  listRecentPowerCaptures:
    vi.fn<typeof import("@/lib/powerCaptureStore").listRecentPowerCaptures>(),
  recoverStalePowerCaptureArchives:
    vi.fn<typeof import("@/lib/powerCaptureStore").recoverStalePowerCaptureArchives>(),
  renewPowerCaptureArchiveLease:
    vi.fn<typeof import("@/lib/powerCaptureStore").renewPowerCaptureArchiveLease>(),
}));

export const apiMocks = hoistedApiMocks;
export const powerCaptureStoreMocks = hoistedPowerCaptureStoreMocks;

vi.mock("@/lib/api", () => hoistedApiMocks);
vi.mock("@/lib/powerCaptureStore", async (importOriginal) => ({
  ...(await importOriginal<typeof import("@/lib/powerCaptureStore")>()),
  ...hoistedPowerCaptureStoreMocks,
}));

export interface BoardView {
  current: () => UseBoard;
  close: () => void;
}

export class MockWebSocket {
  static readonly CONNECTING = 0;
  static readonly OPEN = 1;
  static readonly CLOSING = 2;
  static readonly CLOSED = 3;
  static instances: MockWebSocket[] = [];
  readyState = MockWebSocket.CONNECTING;
  readonly sent: string[] = [];
  onopen: (() => void) | null = null;
  onmessage: ((event: { readonly data: string }) => void) | null = null;
  onerror: (() => void) | null = null;
  onclose: (() => void) | null = null;

  constructor(readonly url: string) {
    MockWebSocket.instances.push(this);
  }

  send(data: string): void {
    this.sent.push(data);
  }

  close(): void {
    this.readyState = MockWebSocket.CLOSED;
  }

  emitOpen(): void {
    this.readyState = MockWebSocket.OPEN;
    this.onopen?.();
  }

  emitMessage(message: unknown): void {
    this.onmessage?.({ data: JSON.stringify(message) });
  }

  emitClose(): void {
    this.readyState = MockWebSocket.CLOSED;
    this.onclose?.();
  }
}

let hiddenState = false;
Object.defineProperty(document, "hidden", {
  configurable: true,
  get: () => hiddenState,
});

export function setHidden(hidden: boolean): void {
  hiddenState = hidden;
  act(() => {
    document.dispatchEvent(new Event("visibilitychange"));
  });
}

const scheduledFrames = new Map<number, FrameRequestCallback>();
let nextFrameHandle = 1;

export const scheduledFrameCount = (): number => scheduledFrames.size;

export function flushNextFrame(): void {
  const entry = scheduledFrames.entries().next();
  if (entry.done) throw new Error("no scheduled animation frame");
  const [handle, callback] = entry.value;
  scheduledFrames.delete(handle);
  act(() => callback(performance.now()));
}

export function setupUseBoardHarness(): void {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.stubGlobal("WebSocket", MockWebSocket);
  vi.stubGlobal("requestAnimationFrame", (callback: FrameRequestCallback): number => {
    const handle = nextFrameHandle;
    nextFrameHandle += 1;
    scheduledFrames.set(handle, callback);
    return handle;
  });
  vi.stubGlobal("cancelAnimationFrame", (handle: number): void => {
    scheduledFrames.delete(handle);
  });
  vi.resetAllMocks();
  apiMocks.createLiveSession.mockResolvedValue({
    session_id: 1,
    ws_url: "/api/v1/live/1",
    connected: true,
  });
  apiMocks.deleteLiveSession.mockResolvedValue(undefined);
  apiMocks.getAdc.mockResolvedValue({ readings: [] });
  apiMocks.getStatus.mockResolvedValue({
    power_capture_protocol: "host-stream-v1",
    power_outputs: [],
    gpios: [],
    switches: {},
    watchdog: {},
    board_monitoring: {},
  });
  apiMocks.liveWebSocketUrl.mockImplementation(
    (path) => `ws://fixture.invalid${path}`,
  );
  powerCaptureStoreMocks.appendPowerCaptureChunk.mockResolvedValue({
    chunkCount: 0,
    persistedSamples: 0,
    estimatedBytes: 0,
    lastSequence: 0,
  });
  powerCaptureStoreMocks.beginPowerCaptureArchive.mockResolvedValue(undefined);
  powerCaptureStoreMocks.clearPowerCaptureArchives.mockResolvedValue(0);
  powerCaptureStoreMocks.deletePowerCaptureArchive.mockResolvedValue(undefined);
  powerCaptureStoreMocks.ensurePowerCaptureStorageCapacity.mockResolvedValue({
    sampleCount: null,
    projectedBytes: null,
    usageBytes: null,
    quotaBytes: null,
    availableBytes: null,
    reserveBytes: null,
    sufficient: true,
    persisted: null,
  });
  powerCaptureStoreMocks.listRecentPowerCaptures.mockResolvedValue([]);
  powerCaptureStoreMocks.recoverStalePowerCaptureArchives.mockResolvedValue([]);
  powerCaptureStoreMocks.renewPowerCaptureArchiveLease.mockResolvedValue(undefined);
}

export function teardownUseBoardHarness(): void {
  vi.unstubAllGlobals();
  vi.clearAllMocks();
  MockWebSocket.instances = [];
  scheduledFrames.clear();
  nextFrameHandle = 1;
  hiddenState = false;
}

export async function flushMicrotasks(): Promise<void> {
  for (let index = 0; index < 6; index += 1) {
    await act(async () => {
      await Promise.resolve();
    });
  }
}

export function renderBoard(): BoardView {
  let current: UseBoard | null = null;
  const host = document.createElement("div");
  const root: Root = createRoot(host);
  function Probe() {
    current = useBoard();
    return null;
  }
  act(() => root.render(<Probe />));
  return {
    current: (): UseBoard => {
      if (current == null) throw new TypeError("useBoard did not render");
      return current;
    },
    close: () => act(() => root.unmount()),
  };
}

export async function renderLiveBoard(): Promise<{ view: BoardView; socket: MockWebSocket }> {
  const view = renderBoard();
  await flushMicrotasks();
  act(() => view.current().setLive(true));
  await flushMicrotasks();
  const socket = MockWebSocket.instances.at(-1);
  if (!socket) throw new Error("live WebSocket was not created");
  act(() => socket.emitOpen());
  return { view, socket };
}

export function currentReading(value: number) {
  return {
    name: "5v_out",
    signal: "S_C_5V",
    kind: "current",
    unit: "uA",
    value,
    power_enabled: true,
  };
}

export function telemetryMessage(value: number, sequence: number) {
  return {
    type: "telemetry",
    topic: "adc",
    readings: [currentReading(value)],
    sample_sequence: sequence,
    device_t_mono_us: sequence * 2000,
  };
}

export function telemetryBatchMessage(sequences: readonly number[]) {
  return {
    type: "telemetry-batch",
    topic: "adc",
    dropped_samples: 0,
    channels: [{ name: "5v_out", signal: "S_C_5V", kind: "current", unit: "uA" }],
    samples: sequences.map((sequence) => ({
      sample_sequence: sequence,
      device_t_mono_us: sequence * 2000,
      power_enabled_mask: 1,
      values: [sequence * 100],
    })),
  };
}
