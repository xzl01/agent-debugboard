import { describe, expect, it } from "vitest";
import { hostOrigin, hostSerialLogDownloadUrl } from "./hostSerialLogs";

describe("host serial log URL selection", () => {
  it("uses the resident Host origin when the Web UI is served by Host", () => {
    expect(hostOrigin({ protocol: "http:", hostname: "127.0.0.1", port: "18790", origin: "http://127.0.0.1:18790" } as Location, false)).toBe("http://127.0.0.1:18790");
  });

  it("uses the loopback Host from Vite, board-hosted and Pages builds", () => {
    expect(hostOrigin({ protocol: "http:", hostname: "127.0.0.1", port: "5173", origin: "http://127.0.0.1:5173" } as Location, true)).toBe("http://127.0.0.1:8787");
    expect(hostOrigin({ protocol: "https:", hostname: "example.com", port: "", origin: "https://example.com" } as Location, false)).toBe("http://127.0.0.1:8787");
  });

  it("encodes session IDs in download links", () => {
    expect(hostSerialLogDownloadUrl("session/id", "raw")).toContain("session%2Fid/download?format=raw");
  });
});
