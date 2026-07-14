// Thin client for the Radxa Linkr Debugger firmware REST API.
// All paths are served under /api/v1 (proxied to the board by the dev server).

const BASE = "/api/v1";

export interface LiveSession {
  session_id: number;
  ws_url: string;
  connected: boolean;
}

export class BoardApiError extends Error {
  code?: string;
  constructor(message: string, code?: string) {
    super(message);
    this.name = "BoardApiError";
    this.code = code;
  }
}

async function request<T = any>(path: string, init?: RequestInit): Promise<T> {
  let res: Response;
  try {
    res = await fetch(BASE + path, {
      headers: { "Content-Type": "application/json" },
      ...init,
    });
  } catch (e) {
    throw new BoardApiError(
      e instanceof Error ? e.message : "Network request failed"
    );
  }

  const text = await res.text();
  const data = text ? JSON.parse(text) : {};

  if (!res.ok || data.ok === false) {
    const err = data?.error;
    throw new BoardApiError(
      err?.message || `HTTP ${res.status} ${res.statusText}`,
      err?.code
    );
  }
  return data as T;
}

export const getStatus = () => request("/status");
export const getAdc = () => request("/adc/read");

export const setPower = (name: string, on: boolean) =>
  request(`/power/${encodeURIComponent(name)}`, {
    method: "PUT",
    body: JSON.stringify({ state: on ? "on" : "off" }),
  });

export const setSwitch = (name: "sd" | "usb", route: string) =>
  request(`/switch/${name}`, {
    method: "PUT",
    body: JSON.stringify({ route }),
  });

export const setGpio = (
  identifier: string,
  direction: "input" | "output",
  value?: number
) =>
  request(`/gpio/${encodeURIComponent(identifier)}`, {
    method: "PUT",
    body: JSON.stringify(
      direction === "output" ? { direction, value: value ? 1 : 0 } : { direction }
    ),
  });

export const getWatchdog = () => request("/watchdog");

export const createLiveSession = () =>
  request<LiveSession>("/live-sessions", { method: "POST" });

export const deleteLiveSession = (sessionId: number) =>
  request(`/live-sessions/${sessionId}`, { method: "DELETE" });

export const enterBootloader = () =>
  request("/bootloader", { method: "POST" });
