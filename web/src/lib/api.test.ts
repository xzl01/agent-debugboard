import { afterEach, describe, expect, it, vi } from "vitest";
import { enterBootloader, isPrivateSlash30, rebootBoard, setGpio, setNetIdentity, setPower, setSwitch } from "./api";

function response(): Response {
  return new Response("{}", { status: 200, statusText: "OK" });
}

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
