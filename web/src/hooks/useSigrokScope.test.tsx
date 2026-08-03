import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import * as api from "@/lib/api";
import type {
  SigrokClientEvent,
  SigrokClientEventListener,
  SigrokClientState,
} from "@/lib/sigrokClient";
import { useSigrokScope, type UseSigrokScopeReturn } from "./useSigrokScope";

const sigrokMocks = vi.hoisted(() => {
  class MockSigrokClient {
    static instances: MockSigrokClient[] = [];
    state: SigrokClientState = "disconnected";
    listeners: SigrokClientEventListener[] = [];
    connect = vi.fn(async () => {
      this.emitState("ready");
    });

    constructor() {
      MockSigrokClient.instances.push(this);
    }

    addEventListener(listener: SigrokClientEventListener) {
      this.listeners.push(listener);
    }

    removeEventListener(listener: SigrokClientEventListener) {
      this.listeners = this.listeners.filter((candidate) => candidate !== listener);
    }

    getState() {
      return this.state;
    }

    getServerCapabilities() {
      return { hello: null, caps: null, serverFlags: 0, supportsConfigV2: false, supportsGenericPackedBurst: false };
    }

    disconnect() {
      this.emitState("disconnected");
    }

    emitState(state: SigrokClientState) {
      this.state = state;
      const event: SigrokClientEvent = { type: "state", state };
      for (const listener of this.listeners) listener(event);
    }

    configure = vi.fn();
    start = vi.fn();
    stop = vi.fn();
    getCaps = vi.fn();
  }

  return { MockSigrokClient };
});

vi.mock("@/lib/sigrokClient", async (importOriginal) => ({
  ...(await importOriginal<typeof import("@/lib/sigrokClient")>()),
  SigrokClient: sigrokMocks.MockSigrokClient,
}));

vi.mock("@/lib/api", () => ({
  createLiveSession: vi.fn(),
  deleteLiveSession: vi.fn(),
  liveWebSocketUrl: vi.fn((path: string) => `ws://fixture.invalid${path}`),
}));

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

function deferred<T>() {
  let resolve: (value: T) => void = () => {};
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
}

function renderHook() {
  let current: UseSigrokScopeReturn | null = null;
  const host = document.createElement("div");
  const root = createRoot(host);
  function Probe() {
    current = useSigrokScope();
    return null;
  }
  act(() => root.render(<Probe />));
  return {
    current: () => {
      if (current == null) throw new TypeError("Hook did not render");
      return current;
    },
    close: () => act(() => root.unmount()),
  };
}

async function flush() {
  await act(async () => { await Promise.resolve(); });
}

describe("useSigrokScope live-session lifecycle", () => {
  afterEach(() => {
    vi.clearAllMocks();
    sigrokMocks.MockSigrokClient.instances = [];
  });

  it("coalesces concurrent session creation and connection attempts", async () => {
    const created = deferred<api.LiveSession>();
    vi.mocked(api.createLiveSession).mockReturnValue(created.promise);
    vi.mocked(api.deleteLiveSession).mockResolvedValue(undefined);
    const view = renderHook();

    let first!: Promise<void>;
    let second!: Promise<void>;
    act(() => {
      first = view.current().ensureConnected();
      second = view.current().ensureConnected();
    });
    expect(api.createLiveSession).toHaveBeenCalledTimes(1);

    created.resolve({ session_id: 7, ws_url: "/api/v1/ws/7", connected: false });
    await act(async () => { await Promise.all([first, second]); });

    const client = sigrokMocks.MockSigrokClient.instances[0];
    expect(client?.connect).toHaveBeenCalledTimes(1);
    expect(client?.connect).toHaveBeenCalledWith("ws://fixture.invalid/api/v1/ws/7");
    view.close();
  });

  it("cancels a pending live-session connection when the hook unmounts", async () => {
    const created = deferred<api.LiveSession>();
    vi.mocked(api.createLiveSession).mockReturnValue(created.promise);
    vi.mocked(api.deleteLiveSession).mockResolvedValue(undefined);
    const view = renderHook();

    let connection!: Promise<void>;
    act(() => {
      connection = view.current().ensureConnected();
    });
    const rejected = expect(connection).rejects.toThrow("Connection cancelled");

    view.close();
    created.resolve({ session_id: 9, ws_url: "/api/v1/ws/9", connected: false });
    await act(async () => {
      await rejected;
      await Promise.resolve();
    });

    const client = sigrokMocks.MockSigrokClient.instances[0];
    expect(client?.connect).not.toHaveBeenCalled();
    expect(api.deleteLiveSession).toHaveBeenCalledTimes(1);
    expect(api.deleteLiveSession).toHaveBeenCalledWith(9);
  });

  it("does not delete a replacement session when an older creation resolves late", async () => {
    const firstCreated = deferred<api.LiveSession>();
    const secondCreated = deferred<api.LiveSession>();
    vi.mocked(api.createLiveSession)
      .mockReturnValueOnce(firstCreated.promise)
      .mockReturnValueOnce(secondCreated.promise);
    vi.mocked(api.deleteLiveSession).mockResolvedValue(undefined);
    const view = renderHook();

    let first!: Promise<void>;
    act(() => {
      first = view.current().ensureConnected();
      view.current().close();
    });
    const firstRejected = expect(first).rejects.toThrow("Connection cancelled");

    let second!: Promise<void>;
    act(() => {
      second = view.current().ensureConnected();
    });
    secondCreated.resolve({ session_id: 22, ws_url: "/api/v1/ws/22", connected: false });
    await act(async () => { await second; });

    firstCreated.resolve({ session_id: 21, ws_url: "/api/v1/ws/21", connected: false });
    await act(async () => { await firstRejected; });

    expect(api.deleteLiveSession).toHaveBeenCalledWith(21);
    expect(api.deleteLiveSession).not.toHaveBeenCalledWith(22);
    view.close();
  });

  it("invalidates a remotely closed session so one retry creates a replacement", async () => {
    vi.mocked(api.createLiveSession)
      .mockResolvedValueOnce({ session_id: 11, ws_url: "/api/v1/ws/11", connected: false })
      .mockResolvedValueOnce({ session_id: 12, ws_url: "/api/v1/ws/12", connected: false });
    vi.mocked(api.deleteLiveSession).mockResolvedValue(undefined);
    const view = renderHook();

    await act(async () => { await view.current().ensureConnected(); });
    const client = sigrokMocks.MockSigrokClient.instances[0];
    expect(client).toBeDefined();

    act(() => { client?.emitState("disconnected"); });
    await flush();
    expect(api.deleteLiveSession).toHaveBeenCalledWith(11);

    await act(async () => { await view.current().ensureConnected(); });
    expect(api.createLiveSession).toHaveBeenCalledTimes(2);
    expect(client?.connect).toHaveBeenLastCalledWith("ws://fixture.invalid/api/v1/ws/12");
    view.close();
  });
});
