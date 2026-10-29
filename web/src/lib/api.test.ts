import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { BOARD_READ_TIMEOUT_MS, BoardApiError, enterBootloader, getAdc, getPd, getStatus, isPrivateSlash30, rebootBoard, setGpio, setNetIdentity, setPower, setSwitch } from "./api";

function response(): Response {
  return new Response("{}", { status: 200, statusText: "OK" });
}

describe("short board read deadlines", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => {
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  it.each([getStatus, getAdc])("aborts hung headers with a stable timeout error (%#)", async (read) => {
    const fetchMock = vi.fn<typeof fetch>(() => new Promise(() => {}));
    vi.stubGlobal("fetch", fetchMock);
    const caller = new AbortController();
    const remove = vi.spyOn(caller.signal, "removeEventListener");
    const pending = read(caller.signal);
    const failed = expect(pending).rejects.toMatchObject({ name: "BoardApiError", code: "timeout" });
    const signal = fetchMock.mock.calls[0][1]!.signal!;
    expect(signal).not.toBe(caller.signal);
    await vi.advanceTimersByTimeAsync(BOARD_READ_TIMEOUT_MS - 1);
    expect(signal.aborted).toBe(false);
    await vi.advanceTimersByTimeAsync(1);
    await failed;
    expect(signal.aborted).toBe(true);
    expect(signal.reason).toBeInstanceOf(BoardApiError);
    expect(caller.signal.aborted).toBe(false);
    expect(remove).toHaveBeenCalledWith("abort", expect.any(Function));
    expect(vi.getTimerCount()).toBe(0);
  });

  it.each([getStatus, getAdc])("covers headers and a hung body with one total budget (%#)", async (read) => {
    const accepted = response();
    const body = vi.spyOn(accepted, "text").mockImplementation(() => new Promise(() => {}));
    const fetchMock = vi.fn<typeof fetch>(() => new Promise((resolve) => {
      setTimeout(() => resolve(accepted), 4000);
    }));
    vi.stubGlobal("fetch", fetchMock);
    const pending = read();
    const failed = expect(pending).rejects.toMatchObject({ code: "timeout" });
    await vi.advanceTimersByTimeAsync(4000);
    expect(body).toHaveBeenCalledOnce();
    await vi.advanceTimersByTimeAsync(BOARD_READ_TIMEOUT_MS - 4001);
    expect(fetchMock.mock.calls[0][1]!.signal!.aborted).toBe(false);
    await vi.advanceTimersByTimeAsync(1);
    await failed;
    expect(fetchMock.mock.calls[0][1]!.signal!.aborted).toBe(true);
    expect(vi.getTimerCount()).toBe(0);
  });

  it.each(["headers", "body"])("preserves caller cancellation during %s and cleans the listener/deadline", async (phase) => {
    const caller = new AbortController();
    const add = vi.spyOn(caller.signal, "addEventListener");
    const remove = vi.spyOn(caller.signal, "removeEventListener");
    const accepted = response();
    const body = vi.spyOn(accepted, "text").mockImplementation(() => new Promise(() => {}));
    const fetchMock = vi.fn<typeof fetch>(() => phase === "headers" ? new Promise(() => {}) : Promise.resolve(accepted));
    vi.stubGlobal("fetch", fetchMock);
    const pending = getStatus(caller.signal);
    const reason = new DOMException("User canceled the read", "AbortError");
    const failed = expect(pending).rejects.toBe(reason);
    await Promise.resolve();
    expect(body).toHaveBeenCalledTimes(phase === "body" ? 1 : 0);
    caller.abort(reason);
    await failed;
    expect(fetchMock.mock.calls[0][1]!.signal!.reason).toBe(reason);
    expect(remove).toHaveBeenCalledWith("abort", add.mock.calls[0][1]);
    expect(vi.getTimerCount()).toBe(0);
  });

  it("does not start fetch or a timer for an already aborted caller", async () => {
    const caller = new AbortController();
    caller.abort();
    const fetchMock = vi.fn<typeof fetch>();
    vi.stubGlobal("fetch", fetchMock);
    await expect(getAdc(caller.signal)).rejects.toBe(caller.signal.reason);
    expect(fetchMock).not.toHaveBeenCalled();
    expect(vi.getTimerCount()).toBe(0);
  });

  it.each(["success", "network error", "invalid JSON"])("cleans resources after %s", async (outcome) => {
    const caller = new AbortController();
    const remove = vi.spyOn(caller.signal, "removeEventListener");
    const fetchMock = vi.fn<typeof fetch>(async () => {
      if (outcome === "network error") throw new TypeError("offline");
      return outcome === "success" ? response() : new Response("not JSON");
    });
    vi.stubGlobal("fetch", fetchMock);
    if (outcome === "success") await expect(getStatus(caller.signal)).resolves.toEqual({});
    else await expect(getStatus(caller.signal)).rejects.toBeInstanceOf(BoardApiError);
    expect(remove).toHaveBeenCalledWith("abort", expect.any(Function));
    expect(vi.getTimerCount()).toBe(0);
    caller.abort();
    expect(fetchMock.mock.calls[0][1]!.signal!.aborted).toBe(false);
  });

  it("does not impose the telemetry deadline on PD reads or power mutations", async () => {
    let finish!: () => void;
    const gate = new Promise<void>((resolve) => { finish = resolve; });
    const fetchMock = vi.fn<typeof fetch>(async () => { await gate; return response(); });
    vi.stubGlobal("fetch", fetchMock);
    const pending = [getPd(), setPower("5v_out", true)];
    await vi.advanceTimersByTimeAsync(BOARD_READ_TIMEOUT_MS * 2);
    expect(vi.getTimerCount()).toBe(0);
    expect(fetchMock.mock.calls.every(([, init]) => !init?.signal)).toBe(true);
    finish();
    await Promise.all(pending);
  });

  it("leaves no deadline on the lifecycle acknowledgement fast path", async () => {
    const accepted = new Response("incomplete", { status: 202 });
    const body = vi.spyOn(accepted, "text").mockImplementation(() => new Promise(() => {}));
    const caller = new AbortController();
    const add = vi.spyOn(caller.signal, "addEventListener");
    vi.stubGlobal("fetch", vi.fn<typeof fetch>(async () => accepted));
    await expect(rebootBoard(caller.signal)).resolves.toBeUndefined();
    expect(body).not.toHaveBeenCalled();
    expect(add).not.toHaveBeenCalled();
    expect(vi.getTimerCount()).toBe(0);
  });
});

describe("dynamic API path segments", () => {
  afterEach(() => vi.unstubAllGlobals());

  it("keeps slash, query, and percent names inside one encoded path segment", async () => {
    const fetchMock = vi.fn(async () => response());
    vi.stubGlobal("fetch", fetchMock);

    const names = [
      { name: "a/b", encoded: "a%2Fb" },
      { name: "a?b", encoded: "a%3Fb" },
      { name: "a%b", encoded: "a%25b" },
    ];

    for (const { name, encoded } of names) {
      await setPower(name, true);
      expect(fetchMock).toHaveBeenLastCalledWith(
        `/api/v1/power/${encoded}`,
        expect.objectContaining({ method: "PUT" }),
      );

      await setSwitch(name, "target");
      expect(fetchMock).toHaveBeenLastCalledWith(
        `/api/v1/switch/${encoded}`,
        expect.objectContaining({ method: "PUT" }),
      );

      await setGpio(name, "input");
      expect(fetchMock).toHaveBeenLastCalledWith(
        `/api/v1/gpio/${encoded}`,
        expect.objectContaining({ method: "PUT" }),
      );
    }
  });

  it("rejects empty and dot path segments before fetch", async () => {
    const fetchMock = vi.fn(async () => response());
    vi.stubGlobal("fetch", fetchMock);

    for (const name of ["", ".", ".."] as const) {
      await expect(setPower(name, true)).rejects.toMatchObject({
        name: "BoardApiError",
        code: "invalid_path",
      });
      await expect(setSwitch(name, "target")).rejects.toMatchObject({
        name: "BoardApiError",
        code: "invalid_path",
      });
      await expect(setGpio(name, "input")).rejects.toMatchObject({
        name: "BoardApiError",
        code: "invalid_path",
      });
    }

    expect(fetchMock).not.toHaveBeenCalled();
  });
});

describe("normal restart API", () => {
  afterEach(() => vi.unstubAllGlobals());

  it("posts once to /reboot with no body and accepts 202 without refreshing or entering BOOTSEL", async () => {
    const fetchMock = vi.fn<typeof fetch>(async () => new Response('{"ok":true}', { status: 202 }));
    vi.stubGlobal("fetch", fetchMock);
    const controller = new AbortController();
    await rebootBoard(controller.signal);
    expect(fetchMock).toHaveBeenCalledExactlyOnceWith("/api/v1/reboot", expect.objectContaining({ method: "POST", signal: controller.signal }));
    expect(fetchMock.mock.calls[0]?.[1]).not.toHaveProperty("body");
  });

  it("rejects an unexpected 200 instead of claiming that the restart was accepted", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => response()));
    await expect(rebootBoard()).rejects.toMatchObject({ code: "invalid_response" });
  });

  it("accepts 202 headers without waiting for a body that can be cut short by the restart", async () => {
    const accepted = new Response("truncated response", { status: 202 });
    const readBody = vi.spyOn(accepted, "text").mockRejectedValue(new TypeError("Restart closed the connection"));
    vi.stubGlobal("fetch", vi.fn(async () => accepted));
    await expect(rebootBoard()).resolves.toBeUndefined();
    expect(readBody).not.toHaveBeenCalled();
  });

  it("preserves firmware refusal and keeps ROM BOOTSEL on its separate endpoint", async () => {
    const fetchMock = vi.fn().mockResolvedValueOnce(new Response('{"ok":false,"error":{"code":"busy","message":"Debugger is busy"}}', { status: 409 })).mockResolvedValueOnce(response());
    vi.stubGlobal("fetch", fetchMock);
    await expect(rebootBoard()).rejects.toMatchObject({ code: "busy", message: "Debugger is busy" });
    await enterBootloader();
    expect(fetchMock).toHaveBeenNthCalledWith(1, "/api/v1/reboot", expect.objectContaining({ method: "POST" }));
    expect(fetchMock).toHaveBeenNthCalledWith(2, "/api/v1/bootloader", expect.objectContaining({ method: "POST" }));
  });
});

describe("network response semantics", () => {
  afterEach(() => vi.unstubAllGlobals());

  it.each([true, false])("preserves explicit reconnect_required=%s and firmware advice through API normalization", async (reconnect_required) => {
    const data = {
      mac: "02:11:22:33:44:55", origin: "manual", network: "10.0.0.4",
      prefix: 30, ip: "10.0.0.5", dhcp_base: "10.0.0.6", url: "http://10.0.0.5/",
      reconnect_required, message: "Renew the host DHCP lease.",
    };
    vi.stubGlobal("fetch", vi.fn(async () => new Response(JSON.stringify(data), { status: 200 })));
    await expect(setNetIdentity({ network: "10.0.0.4" })).resolves.toEqual(data);
  });

  it("keeps missing or malformed optional advice absent so UI fallback remains possible", async () => {
    const data = {
      mac: "02:11:22:33:44:55", origin: "derived", network: "10.0.0.0",
      prefix: 30, ip: "10.0.0.1", dhcp_base: "10.0.0.2", url: "http://10.0.0.1/",
      reconnect_required: "false", message: "  ",
    };
    vi.stubGlobal("fetch", vi.fn(async () => new Response(JSON.stringify(data), { status: 200 })));
    await expect(setNetIdentity({ network: "10.0.0.0" })).resolves.toEqual({
      mac: "02:11:22:33:44:55", origin: "derived", network: "10.0.0.0",
      prefix: 30, ip: "10.0.0.1", dhcp_base: "10.0.0.2", url: "http://10.0.0.1/",
    });
  });
});


describe("private /30 validation", () => {
  it.each(["10.0.0.0", "10.255.255.252", "172.16.0.0", "172.31.255.252", "192.168.0.0"])("accepts %s", (value) => {
    expect(isPrivateSlash30(value)).toBe(true);
  });

  it.each(["8.8.8.0", "172.15.0.0", "172.32.0.0", "192.167.0.0", "10.0.0.1", "172.29.0.2"])("rejects %s", (value) => {
    expect(isPrivateSlash30(value)).toBe(false);
  });
});
