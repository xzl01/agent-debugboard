const DEFAULT_HOST_ORIGIN = "http://127.0.0.1:8787";

export interface HostSerialLogStatus {
  schema: "linkr-serial-log.v1";
  enabled: boolean;
  root: string;
  state: "disabled" | "ready" | "degraded";
  active_sessions: number;
  queued_records: number;
  total_bytes: number;
  quota_bytes: number;
  dropped_bytes: number;
  last_error: string | null;
}

export interface HostSerialLog {
  schema: "linkr-serial-log.v1";
  session_id: string;
  channel: "uart0" | "uart1";
  device_path: string;
  baud: number;
  started_at: string;
  ended_at: string | null;
  status: "recording" | "complete" | "incomplete" | "interrupted";
  complete: boolean;
  pinned: boolean;
  bytes: number;
  records: number;
  segments: number;
  dropped_bytes: number;
  end_reason: string | null;
  directory: string;
}

interface HostSerialLogList {
  schema: "linkr-serial-log-list.v1";
  logs: HostSerialLog[];
}

export function hostOrigin(
  locationValue: Pick<Location, "protocol" | "hostname" | "port" | "origin"> = window.location,
  development = Boolean(import.meta.env?.DEV)
): string {
  if (development) return DEFAULT_HOST_ORIGIN;
  const loopback = locationValue.hostname === "127.0.0.1" || locationValue.hostname === "localhost";
  if (locationValue.protocol === "http:" && loopback) {
    return locationValue.origin;
  }
  return DEFAULT_HOST_ORIGIN;
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(`${hostOrigin()}${path}`, {
    ...init,
    headers: {
      ...(init?.body ? { "Content-Type": "application/json" } : {}),
      ...init?.headers,
    },
  });
  if (!response.ok) {
    let message = `HTTP ${response.status} ${response.statusText}`;
    try {
      const body = await response.json();
      message = body?.error?.message || message;
    } catch {
      // Keep the HTTP fallback when Host returned a non-JSON error.
    }
    throw new Error(message);
  }
  return response.json() as Promise<T>;
}

export const getHostSerialLogStatus = () =>
  request<HostSerialLogStatus>("/host/api/v1/serial-logging/status");

export const listHostSerialLogs = async () =>
  (await request<HostSerialLogList>("/host/api/v1/serial-logs")).logs;

export const setHostSerialLogPinned = (sessionId: string, pinned: boolean) =>
  request<HostSerialLog>(`/host/api/v1/serial-logs/${encodeURIComponent(sessionId)}/pin`, {
    method: "PUT",
    body: JSON.stringify({ pinned }),
  });

export async function deleteHostSerialLog(sessionId: string): Promise<void> {
  const response = await fetch(
    `${hostOrigin()}/host/api/v1/serial-logs/${encodeURIComponent(sessionId)}?confirm=true`,
    { method: "DELETE" }
  );
  if (!response.ok) {
    const body = await response.json().catch(() => null);
    throw new Error(body?.error?.message || `HTTP ${response.status} ${response.statusText}`);
  }
}

export function hostSerialLogDownloadUrl(
  sessionId: string,
  format: "raw" | "text" | "ndjson"
): string {
  return `${hostOrigin()}/host/api/v1/serial-logs/${encodeURIComponent(sessionId)}/download?format=${format}`;
}
