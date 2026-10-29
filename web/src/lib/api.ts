import { parsePersistentConfigError, parsePersistentConfigGet, parsePersistentConfigMutation } from "./persistentConfig.ts";
import type { PersistentConfig } from "./persistentConfig.ts";
import { createTaskV2Client, type TaskStoreMetadata } from "./taskV2.ts";
import { isTaskControlPath } from "./taskBlob.ts";
import { parseTaskCatalogResponse, type BuiltInTask } from "./builtinTasks.ts";
import { parseGpioPhysicalLayout } from "./gpioPhysicalLayout.ts";

// Thin client for the Radxa Linkr Debugger firmware REST API. Local development
// uses Vite's same-origin proxy. The Pages build points this at the loopback
// Linkr Host gateway started by `npm run host`.

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

// Only short status/ADC reads opt into this total (headers + body) deadline.
// PD discovery, mutations, lifecycle operations and the separate OTA/task/blob
// clients retain their existing operation-specific lifetime, not a short cap.
export const BOARD_READ_TIMEOUT_MS = 10_000;

async function request<T = unknown>(path: string, init?: RequestInit, acknowledgeStatus?: number, timeoutMs?: number): Promise<T> {
  if (timeoutMs === undefined) return fetchJson<T>(path, init, acknowledgeStatus);

  const callerSignal = init?.signal;
  callerSignal?.throwIfAborted();
  const controller = new AbortController();
  let rejectAborted!: (reason: unknown) => void;
  const aborted = new Promise<never>((_, reject) => { rejectAborted = reject; });
  const abort = (reason: unknown) => {
    // Reject independently of fetch/body cancellation: even a transport that
    // fails to settle on abort must not permanently stop completion-based polls.
    rejectAborted(reason);
    controller.abort(reason);
  };
  const onCallerAbort = () => abort(callerSignal?.reason);
  callerSignal?.addEventListener("abort", onCallerAbort, { once: true });
  const timer = setTimeout(() => abort(new BoardApiError("Board API request timed out.", "timeout")), timeoutMs);
  try {
    return await Promise.race([
      fetchJson<T>(path, { ...init, signal: controller.signal }, acknowledgeStatus),
      aborted,
    ]);
  } finally {
    clearTimeout(timer);
    callerSignal?.removeEventListener("abort", onCallerAbort);
  }
}

async function fetchJson<T>(path: string, init?: RequestInit, acknowledgeStatus?: number): Promise<T> {
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
        "If this page is hosted on GitHub Pages, start Linkr Host with `npm run host`."
    );
  }

  // A lifecycle POST is accepted at its acknowledgement status. The board can
  // drop the transport immediately afterwards; do not wait for its JSON body.
  if (acknowledgeStatus !== undefined && res.status === acknowledgeStatus) {
    void res.body?.cancel().catch(() => {});
    return undefined as T;
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
  if (acknowledgeStatus !== undefined && res.status !== acknowledgeStatus) {
    throw new BoardApiError(`Device endpoint returned ${res.status}; expected ${acknowledgeStatus}.`, "invalid_response", data);
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

export const getStatus = (signal?: AbortSignal) => request("/status", { signal }, undefined, BOARD_READ_TIMEOUT_MS);
export const getGpioLayout = async (signal?: AbortSignal) =>
  parseGpioPhysicalLayout(await request("/gpio/layout", { signal }));
export const getAdc = (signal?: AbortSignal) => request("/adc/read", { signal }, undefined, BOARD_READ_TIMEOUT_MS);
export const getPd = () => request("/pd");
export const setPd = (mode: 0 | 1, voltageMv: number) =>
  request("/pd", {
    method: "PUT",
    body: JSON.stringify({ mode, voltage_mv: voltageMv }),
  });

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

// Normal cold restart is deliberately separate from ROM BOOTSEL entry.
export const rebootBoard = async (signal?: AbortSignal): Promise<void> => {
  await request<void>("/reboot", { method: "POST", signal }, 202);
};

const taskClient = createTaskV2Client(BASE);
export const getTaskMetadata = taskClient.list;
export const getTaskIr = taskClient.readIr;
export const getTaskSource = taskClient.readSource;
export const mutateTasks = taskClient.mutate;
export const upsertWorkflowTask = taskClient.upsertSource;
export const getTasks = async (signal?: AbortSignal) => {
  const snapshot = await taskClient.readIr(signal);
  return { ...snapshot, tasks: snapshot.metadata.tasks.map((task) => ({ ...task, name: task.id })) };
};

export const getTaskCatalog = async (signal?: AbortSignal): Promise<readonly BuiltInTask[]> =>
  parseTaskCatalogResponse(await request<unknown>("/tasks/catalog", { signal }));

export const importTaskIr = (blob: string, metadata: TaskStoreMetadata, signal?: AbortSignal) =>
  taskClient.mutate({ action: "import", ir: blob, expectedRevision: metadata.revision, catalogFingerprint: metadata.catalog_fingerprint }, signal);
export const clearTasks = async (metadata?: TaskStoreMetadata) => {
  const current = metadata ?? await taskClient.list();
  return taskClient.mutate({ action: "clear", expectedRevision: current.revision, catalogFingerprint: current.catalog_fingerprint });
};
export const deleteTask = async (id: string, metadata?: TaskStoreMetadata) => {
  const current = metadata ?? await taskClient.list();
  return taskClient.mutate({ action: "delete", id, expectedRevision: current.revision, catalogFingerprint: current.catalog_fingerprint });
};

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

export type NetOrigin = "derived" | "manual";

export interface NetSettings {
  readonly mac: string;
  readonly origin: NetOrigin;
  readonly network: string;
  readonly prefix: 30;
  readonly ip: string;
  readonly dhcp_base: string;
  readonly url: string;
  readonly reconnect_required?: boolean;
  readonly message?: string;
}

const IPV4_PATTERN = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/;

export function parseNetworkBase(value: string): number | null {
  const match = IPV4_PATTERN.exec(value.trim());
  if (!match) return null;
  const octets = match.slice(1).map(Number);
  if (octets.some((octet) => octet < 0 || octet > 255)) return null;
  return ((octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3]) >>> 0;
}

export function isPrivateSlash30(value: string): boolean {
  const address = parseNetworkBase(value);
  if (address === null || (address & 3) !== 0) return false;
  const first = address >>> 24;
  const second = (address >>> 16) & 0xff;
  return first === 10 ||
    (first === 172 && second >= 16 && second <= 31) ||
    (first === 192 && second === 168);
}

export function isLocallyAdministeredMac(value: string): boolean {
  const parts = value.trim().toLowerCase().split(":");
  if (parts.length !== 6 || parts.some((part) => !/^[0-9a-f]{2}$/.test(part))) return false;
  return (Number.parseInt(parts[0], 16) & 0x03) === 0x02;
}

function parseNetSettings(data: unknown): NetSettings {
  const record = asRecord(data);
  const mac = record?.mac;
  const origin = record?.origin;
  const network = record?.network;
  const prefix = record?.prefix;
  const ip = record?.ip;
  const dhcpBase = record?.dhcp_base;
  const url = record?.url;
  if (
    typeof mac !== "string" || !isLocallyAdministeredMac(mac) ||
    (origin !== "derived" && origin !== "manual") ||
    typeof network !== "string" || !isPrivateSlash30(network) ||
    prefix !== 30 ||
    typeof ip !== "string" || !IPV4_PATTERN.test(ip) ||
    typeof dhcpBase !== "string" || !IPV4_PATTERN.test(dhcpBase) ||
    typeof url !== "string" || !/^http:\/\/[^/]+\/$/.test(url)
  ) {
    throw new BoardApiError("Device returned an invalid network settings response.", "invalid_response", data);
  }
  return {
    mac,
    origin,
    network,
    prefix,
    ip,
    dhcp_base: dhcpBase,
    url,
    ...(typeof record?.reconnect_required === "boolean" ? { reconnect_required: record.reconnect_required } : {}),
    ...(typeof record?.message === "string" && record.message.trim() ? { message: record.message.trim() } : {}),
  };
}

export const getNetSettings = async (): Promise<NetSettings> =>
  parseNetSettings(await request("/net"));

export interface NetIdentityUpdate {
  readonly mac?: string;
  readonly network?: string;
}

export const setNetIdentity = async (update: NetIdentityUpdate): Promise<NetSettings> =>
  parseNetSettings(await request("/net", {
    method: "PUT",
    body: JSON.stringify(update),
  }));
