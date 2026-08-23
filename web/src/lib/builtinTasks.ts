import { TASK_MAX_REQUESTS, TASK_MAX_WAIT_MS, type TaskRequest } from "./taskRequests.ts";
import {
  isTaskControlPath,
  TaskDataError,
  taskByteLength,
  taskJsonDepthValid,
  TASK_BODY_MAX_BYTES,
  TASK_ID_MAX_BYTES,
  TASK_MAX_JSON_DEPTH,
  TASK_NAME_MAX_BYTES,
} from "./taskBlob.ts";
import type { TaskSummary } from "./taskRunner.ts";

/**
 * Firmware-owned built-in automation tasks, fetched from
 * `GET /api/v1/tasks/catalog` and validated strictly at the boundary. The
 * Web host keeps no hardware recipe constants: rails, GPIO identifiers,
 * levels, and wait timings all come from the firmware response. Built-ins
 * are ephemeral and immutable: they are never written to or deleted through
 * the firmware `/api/v1/tasks` storage API, and they run through the same
 * generic `runTaskRequests` path as stored tasks. On an exact ID collision
 * the built-in wins and the stored entry is shadowed, not deleted.
 */
export interface BuiltInTask {
  readonly id: string;
  readonly name: string;
  readonly requests: readonly TaskRequest[];
  readonly cleanup: TaskRequest;
}

export type TaskCatalogEntry =
  | {
      readonly source: "builtin";
      readonly id: string;
      readonly name: string;
      readonly requestCount: number;
      readonly shadowedStored: boolean;
    }
  | {
      readonly source: "stored";
      readonly id: string;
      readonly name: string;
      readonly requestCount: number;
    };

// Mirrors LINKR_DEBUGGER_TASK_CATALOG_VERSION / _TASK_COUNT and the shared
// radxa-linkr-debugger.v1 envelope schema.
export const TASK_CATALOG_VERSION = 1;
export const TASK_CATALOG_MAX_TASKS = 6;
export const BUILTIN_TASK_ID_PREFIX = "builtin/";
const FROZEN_SCHEMA = "radxa-linkr-debugger.v1";

const CATALOG_ENVELOPE_KEYS = new Set(["schema", "ok", "command", "action", "version", "tasks"]);

const CATALOG_REQUEST_KEYS = new Set(["method", "path", "body", "wait_ms"]);
const CATALOG_TASK_KEYS = new Set(["id", "name", "requests", "cleanup"]);

function asRecord(value: unknown): Record<string, unknown> | null {
  return typeof value === "object" && value !== null && !Array.isArray(value)
    ? Object.fromEntries(Object.entries(value))
    : null;
}

function fail(detail: string): never {
  throw new TaskDataError("invalid_response", detail);
}

function rejectUnknownKeys(
  record: Record<string, unknown>,
  allowed: ReadonlySet<string>,
  context: string,
): void {
  for (const key of Object.keys(record)) {
    if (!allowed.has(key)) fail(`${context} carries unsupported field ${JSON.stringify(key)}`);
  }
}

function parseCatalogRequest(value: unknown, context: string): TaskRequest {
  const record = asRecord(value);
  if (!record) fail(`${context} is not a JSON object`);
  rejectUnknownKeys(record, CATALOG_REQUEST_KEYS, context);
  if (record.method !== "PUT") fail(`${context} must use method PUT`);
  const path = record.path;
  if (typeof path !== "string" || !isTaskControlPath(path)) {
    fail(`${context} path ${JSON.stringify(path)} is not an allowed control path`);
  }
  // Catalog bodies arrive as JSON objects on the wire; the generic executor
  // dispatches canonical JSON strings, so parse-then-stringify once here.
  const body = asRecord(record.body);
  if (!body) fail(`${context} body must be a JSON object`);
  const canonicalBody = JSON.stringify(body);
  if (taskByteLength(canonicalBody) > TASK_BODY_MAX_BYTES) {
    fail(`${context} body exceeds the ${TASK_BODY_MAX_BYTES} byte firmware limit`);
  }
  if (!taskJsonDepthValid(canonicalBody)) {
    fail(`${context} body exceeds the ${TASK_MAX_JSON_DEPTH} level JSON depth limit`);
  }
  const waitMs = record.wait_ms;
  if (waitMs === undefined) return { method: "PUT", path, body: canonicalBody };
  if (
    typeof waitMs !== "number" ||
    !Number.isInteger(waitMs) ||
    waitMs < 0 ||
    waitMs > TASK_MAX_WAIT_MS
  ) {
    fail(`${context} wait_ms must be an integer within 0..${TASK_MAX_WAIT_MS}`);
  }
  return { method: "PUT", path, body: canonicalBody, wait_ms: waitMs };
}

export function parseTaskCatalogResponse(data: unknown): readonly BuiltInTask[] {
  const envelope = asRecord(data);
  if (!envelope) fail("catalog response is not a JSON object");
  rejectUnknownKeys(envelope, CATALOG_ENVELOPE_KEYS, "catalog envelope");
  if (envelope.schema !== FROZEN_SCHEMA) {
    fail(`unexpected schema ${JSON.stringify(envelope.schema)}`);
  }
  if (envelope.ok !== true) fail("catalog response does not report ok:true");
  if (envelope.command !== "task") {
    fail(`unexpected command ${JSON.stringify(envelope.command)}`);
  }
  if (envelope.action !== "catalog") {
    fail(`unexpected action ${JSON.stringify(envelope.action)}`);
  }
  if (envelope.version !== TASK_CATALOG_VERSION) {
    fail(`unexpected catalog version ${JSON.stringify(envelope.version)}`);
  }
  if (!Array.isArray(envelope.tasks)) fail("catalog tasks is not an array");
  if (envelope.tasks.length === 0 || envelope.tasks.length > TASK_CATALOG_MAX_TASKS) {
    fail(`catalog must carry 1..${TASK_CATALOG_MAX_TASKS} tasks, got ${envelope.tasks.length}`);
  }
  const seenIds = new Set<string>();
  return envelope.tasks.map((entry, index): BuiltInTask => {
    const context = `catalog task ${index + 1}`;
    const task = asRecord(entry);
    if (!task) fail(`${context} is not a JSON object`);
    rejectUnknownKeys(task, CATALOG_TASK_KEYS, context);
    if (
      typeof task.id !== "string" ||
      taskByteLength(task.id) === 0 ||
      taskByteLength(task.id) > TASK_ID_MAX_BYTES ||
      /[ \t\r\n#]/.test(task.id)
    ) {
      fail(`${context} carries an invalid id`);
    }
    const id = task.id;
    if (seenIds.has(id)) fail(`catalog carries duplicate task id ${JSON.stringify(id)}`);
    seenIds.add(id);
    if (
      typeof task.name !== "string" ||
      task.name === "" ||
      taskByteLength(task.name) > TASK_NAME_MAX_BYTES
    ) {
      fail(`${context} carries an invalid name`);
    }
    if (!Array.isArray(task.requests) || task.requests.length === 0) {
      fail(`task ${id} requests must be a non-empty array`);
    }
    if (task.requests.length > TASK_MAX_REQUESTS) {
      fail(`task ${id} holds more than ${TASK_MAX_REQUESTS} requests`);
    }
    const requests = task.requests.map((request, requestIndex) =>
      parseCatalogRequest(request, `task ${id} request ${requestIndex + 1}`),
    );
    if (task.cleanup === undefined) fail(`task ${id} is missing cleanup`);
    const cleanup = parseCatalogRequest(task.cleanup, `task ${id} cleanup`);
    return { id, name: task.name, requests, cleanup };
  });
}

export function findBuiltInTask(
  catalog: readonly BuiltInTask[],
  id: string,
): BuiltInTask | undefined {
  return catalog.find((task) => task.id === id);
}

/**
 * Merges firmware catalog built-ins with firmware-reported stored tasks.
 * Order is deterministic: catalog built-ins first, then non-colliding stored
 * tasks in firmware order. `null` builtIns means the catalog fetch/parse
 * failed; the firmware-owned `builtin/` namespace stays reserved either way,
 * so stored `builtin/` entries are never displayed or runnable from the merge.
 * Firmware response parsing stays in the API seam; this function only merges
 * already-parsed values.
 */
export function mergeTaskCatalog(
  builtIns: readonly BuiltInTask[] | null,
  stored: readonly TaskSummary[],
): TaskCatalogEntry[] {
  const catalog = builtIns ?? [];
  const storedIds = new Set(stored.map((task) => task.id));
  const builtInIds = new Set(catalog.map((task) => task.id));
  const builtInEntries: TaskCatalogEntry[] = catalog.map((task) => ({
    source: "builtin",
    id: task.id,
    name: task.name,
    requestCount: task.requests.length,
    shadowedStored: storedIds.has(task.id),
  }));
  const storedEntries: TaskCatalogEntry[] = stored
    .filter((task) => !builtInIds.has(task.id) && !task.id.startsWith(BUILTIN_TASK_ID_PREFIX))
    .map((task) => ({
      source: "stored",
      id: task.id,
      name: task.name,
      requestCount: task.request_count,
    }));
  return [...builtInEntries, ...storedEntries];
}
