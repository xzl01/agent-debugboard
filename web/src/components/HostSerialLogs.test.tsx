// @vitest-environment jsdom

import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { HostSerialLogs } from "./HostSerialLogs";
import * as archiveApi from "@/lib/hostSerialLogs";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({
    t: (key: string) => ({
      "serial.hostLogs.records": "Records: {count}",
      "serial.hostLogs.segments": "Segments: {count}",
      "serial.hostLogs.droppedBytes": "Dropped: {bytes}",
    }[key] ?? key),
  }),
}));

vi.mock("@/lib/hostSerialLogs", () => ({
  getHostSerialLogStatus: vi.fn(),
  listHostSerialLogs: vi.fn(),
  setHostSerialLogPinned: vi.fn(),
  deleteHostSerialLog: vi.fn(),
  hostSerialLogDownloadUrl: vi.fn((id: string, format: string) => `/download/${id}/${format}`),
}));

type View = { host: HTMLDivElement; root: Root };
let view: View | null = null;

const status: archiveApi.HostSerialLogStatus = {
  schema: "linkr-serial-log.v1",
  enabled: true,
  root: "/var/lib/linkr/serial",
  state: "degraded",
  active_sessions: 0,
  queued_records: 0,
  total_bytes: 1024,
  quota_bytes: 4096,
  dropped_bytes: 128,
  last_error: "disk latency",
};

const logs: archiveApi.HostSerialLog[] = [
  {
    schema: "linkr-serial-log.v1",
    session_id: "complete-session",
    channel: "uart0",
    device_path: "/dev/ttyUSB0",
    baud: 115200,
    started_at: "2026-08-12T02:42:00Z",
    ended_at: "2026-08-12T02:44:00Z",
    status: "complete",
    complete: true,
    pinned: true,
    bytes: 4096,
    records: 42,
    segments: 2,
    dropped_bytes: 0,
    end_reason: "closed",
    directory: "/archive/complete-session",
  },
  {
    schema: "linkr-serial-log.v1",
    session_id: "partial-session",
    channel: "uart1",
    device_path: "/dev/ttyUSB1",
    baud: 921600,
    started_at: "2026-08-12T03:10:00Z",
    ended_at: "2026-08-12T03:11:00Z",
    status: "incomplete",
    complete: false,
    pinned: false,
    bytes: 2048,
    records: 12,
    segments: 1,
    dropped_bytes: 128,
    end_reason: "archive queue overflow",
    directory: "/archive/partial-session",
  },
];

async function mount(): Promise<View> {
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  act(() => root.render(<HostSerialLogs />));
  await act(async () => {
    await Promise.resolve();
    await Promise.resolve();
  });
  view = { host, root };
  return view;
}

beforeEach(() => {
  vi.mocked(archiveApi.getHostSerialLogStatus).mockResolvedValue(status);
  vi.mocked(archiveApi.listHostSerialLogs).mockResolvedValue(logs);
  vi.mocked(archiveApi.setHostSerialLogPinned).mockResolvedValue(logs[1]);
  vi.mocked(archiveApi.deleteHostSerialLog).mockResolvedValue(undefined);
  vi.spyOn(window, "confirm").mockReturnValue(true);
});

afterEach(() => {
  if (view) act(() => view?.root.unmount());
  view?.host.remove();
  view = null;
  vi.restoreAllMocks();
  vi.clearAllMocks();
});

describe("HostSerialLogs compact archive inspector", () => {
  it("keeps complete and incomplete integrity explicit and exposes all download formats", async () => {
    const { host } = await mount();
    const complete = host.querySelector('[data-testid="host-serial-log-complete-session"]');
    const partial = host.querySelector('[data-testid="host-serial-log-partial-session"]');

    expect(complete?.getAttribute("data-integrity")).toBe("complete");
    expect(partial?.getAttribute("data-integrity")).toBe("incomplete");
    expect(partial?.textContent).toContain("Records: 12");
    expect(partial?.textContent).toContain("Segments: 1");
    expect(partial?.textContent).toContain("Dropped: 128 B");
    expect(partial?.textContent).toContain("archive queue overflow");
    expect(partial?.querySelector('a[href="/download/partial-session/raw"]')).not.toBeNull();
    expect(partial?.querySelector('a[href="/download/partial-session/text"]')).not.toBeNull();
    expect(partial?.querySelector('a[href="/download/partial-session/ndjson"]')).not.toBeNull();

    const completeDelete = complete?.querySelector('button[aria-label="serial.hostLogs.delete"]');
    expect((completeDelete as HTMLButtonElement).disabled).toBe(true);
  });

  it("uses the existing Host delete endpoint and refreshes the compact archive list", async () => {
    const { host } = await mount();
    const partial = host.querySelector('[data-testid="host-serial-log-partial-session"]');
    const deleteButton = partial?.querySelector('button[aria-label="serial.hostLogs.delete"]');
    if (!(deleteButton instanceof HTMLButtonElement)) throw new TypeError("Delete button missing");

    await act(async () => deleteButton.click());

    expect(archiveApi.deleteHostSerialLog).toHaveBeenCalledWith("partial-session");
    expect(archiveApi.getHostSerialLogStatus).toHaveBeenCalledTimes(2);
    expect(archiveApi.listHostSerialLogs).toHaveBeenCalledTimes(2);
  });
});
