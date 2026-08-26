import { afterEach, describe, expect, it, vi } from "vitest";
import { setGpio, setPower, setSwitch } from "./api";

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
