// Thin client for the Radxa Linkr Debugger firmware REST API. Local development
// uses Vite's same-origin proxy. The Pages build points this at the loopback
// device gateway started by `npm run device-bridge`.

const BASE = import.meta.env?.VITE_DEVICE_API_BASE || "/api/v1";

export interface LiveSession {
  session_id: number;
  ws_url: string;
  connected: boolean;
}

export type TargetRecoveryMode = "qualcomm-edl" | "rockchip-maskrom";

export interface TargetRecoveryResult {
  action: "enter";
  mode: TargetRecoveryMode;
  rail: string;
  active_level: 0 | 1;
  off_ms: number;
  setup_ms: number;
  hold_ms: number;
  release_direction: "input";
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
      ...init,
      headers: {
        ...(init?.body ? { "Content-Type": "application/json" } : {}),
        ...init?.headers,
      },
    });
  } catch (e) {
    throw new BoardApiError(
      `${e instanceof Error ? e.message : "Network request failed"}. ` +
        "If this page is hosted on GitHub Pages, start the local gateway with `npm run device-bridge`."
    );
  }

  const text = await res.text();
  let data: any = {};
  if (text) {
    try {
      data = JSON.parse(text);
    } catch {
      throw new BoardApiError(
        `Device endpoint returned ${res.status} ${res.statusText} instead of JSON. ` +
          "Start the local device gateway and verify the configured API endpoint.",
        "invalid_response"
      );
    }
  }

  if (!res.ok || data.ok === false) {
    const err = data?.error;
    throw new BoardApiError(
      err?.message || `HTTP ${res.status} ${res.statusText}`,
      err?.code
    );
  }
  return data as T;
}

export function liveWebSocketUrl(path: string): string {
  const base = new URL(BASE, location.href);
  const session = new URL(path, base);
  const protocol = base.protocol === "https:" ? "wss:" : "ws:";
  return `${protocol}//${base.host}${session.pathname}${session.search}`;
}

export const apiEndpoint = () => BASE;

export const getStatus = () => request("/status");
export const getAdc = () => request("/adc/read");

export const setPower = (name: string, on: boolean) =>
  request(`/power/${encodeURIComponent(name)}`, {
    method: "PUT",
    body: JSON.stringify({ state: on ? "on" : "off" }),
  });

export const setSwitch = (name: "sd" | "usb" | "vin", route: string) =>
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

export const enterTargetRecovery = (mode: TargetRecoveryMode, rail: string) =>
  request<TargetRecoveryResult>("/target-recovery", {
    method: "POST",
    body: JSON.stringify({ mode, rail }),
  });
