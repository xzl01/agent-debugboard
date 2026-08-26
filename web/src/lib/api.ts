import { parsePersistentConfigError, parsePersistentConfigGet, parsePersistentConfigMutation } from "./persistentConfig.ts";
import type { PersistentConfig } from "./persistentConfig.ts";
import { parseTaskListResponse, type TaskListData } from "./taskRunner.ts";
import { isTaskControlPath } from "./taskBlob.ts";
import { parseTaskCatalogResponse, type BuiltInTask } from "./builtinTasks.ts";

// Thin client for the Radxa Linkr Debugger firmware REST API. Local development
// uses Vite's same-origin proxy. The Pages build points this at the loopback
// device gateway started by `npm run device-bridge`.

const BASE = import.meta.env?.VITE_DEVICE_API_BASE || "/api/v1";

export interface LiveSession {
  session_id: number;
  ws_url: string;
  connected: boolean;
}

interface PowerMutationResponse {
  readonly power_output?: {
    readonly name?: string;
    readonly state?: string;
  };
}

export class BoardApiError extends Error {
  code?: string;
  readonly response?: unknown;
  constructor(message: string, code?: string, response?: unknown) {
    super(message);
    this.name = "BoardApiError";
    this.code = code;
    this.response = response;
  }
}

function asRecord(value: unknown): Record<string, unknown> | null {
  return typeof value === "object" && value !== null && !Array.isArray(value)
    ? Object.fromEntries(Object.entries(value))
    : null;
}

async function request<T = unknown>(path: string, init?: RequestInit): Promise<T> {
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
    if (init?.signal?.aborted) throw e;
    throw new BoardApiError(
      `${e instanceof Error ? e.message : "Network request failed"}. ` +
        "If this page is hosted on GitHub Pages, start the local gateway with `npm run device-bridge`."
    );
  }

  const text = await res.text();
  let data: unknown = {};
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

  const envelope = asRecord(data);
  if (!res.ok || envelope?.ok === false) {
    const error = asRecord(envelope?.error);
    const message = typeof error?.message === "string"
      ? error.message
      : `HTTP ${res.status} ${res.statusText}`;
    const code = typeof error?.code === "string" ? error.code : undefined;
    throw new BoardApiError(
      message,
      code,
      data
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

function encodeDynamicPathSegment(name: string): string {
  if (name === "" || name === "." || name === "..") {
    throw new BoardApiError(`invalid dynamic path segment ${JSON.stringify(name)}`, "invalid_path");
  }
  return encodeURIComponent(name);
}

export const setPower = async (name: string, on: boolean) =>
  request<PowerMutationResponse>(`/power/${encodeDynamicPathSegment(name)}`, {
    method: "PUT",
    body: JSON.stringify({ state: on ? "on" : "off" }),
  });

export const setSwitch = async (name: string, route: string) =>
  request(`/switch/${encodeDynamicPathSegment(name)}`, {
    method: "PUT",
    body: JSON.stringify({ route }),
  });

export const setGpio = async (
  identifier: string,
  direction: "input" | "output",
  value?: number
) =>
  request(`/gpio/${encodeDynamicPathSegment(identifier)}`, {
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

export const getTasks = async (signal?: AbortSignal): Promise<TaskListData> =>
  parseTaskListResponse(await request<unknown>("/tasks", { signal }));

export const getTaskCatalog = async (signal?: AbortSignal): Promise<readonly BuiltInTask[]> =>
  parseTaskCatalogResponse(await request<unknown>("/tasks/catalog", { signal }));

export const storeTaskBlob = (blob: string) =>
  request<unknown>("/tasks", {
    method: "PUT",
    body: blob,
  });

export const clearTasks = () =>
  request<unknown>("/tasks", { method: "DELETE" });

const BOARD_API_PREFIX = "/api/v1";

// Stored task records carry the firmware-visible `/api/v1` path; the plain
// request helper prefixes BASE, so the seam strips the stored prefix and
// forwards the exact stored body. wait_ms never leaves the client.
export const dispatchTaskRequest = (path: string, body: string, signal?: AbortSignal) =>
  isTaskControlPath(path)
    ? request<unknown>(path.slice(BOARD_API_PREFIX.length), { method: "PUT", body, signal })
    : Promise.reject(new BoardApiError(`refusing invalid stored task path ${JSON.stringify(path)}`, "invalid_path"));

async function configRequest(path: string, action: string, init?: RequestInit): Promise<unknown> {
  try {
    return await request<unknown>(path, init);
  } catch (error) {
    if (error instanceof BoardApiError) throw parsePersistentConfigError(error.response, error.message, action);
    throw error;
  }
}

export const getPersistentConfig = async (): Promise<PersistentConfig> =>
  parsePersistentConfigGet(await configRequest("/config", "get"));
export const savePersistentConfig = async (items: readonly string[], confirm: boolean) => {
  parsePersistentConfigMutation(await configRequest("/config", "save", { method: "PUT", body: JSON.stringify({ items, confirm }) }), "save");
};
export const clearPersistentConfig = async () => {
  parsePersistentConfigMutation(await configRequest("/config", "clear", { method: "DELETE" }), "clear");
};
