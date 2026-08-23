import { TASK_MAX_REQUESTS, type TaskRequest } from "./taskRequests.ts";
import {
  taskByteLength,
  TaskDataError,
  TASK_BLOB_MAX_BYTES,
  TASK_ID_MAX_BYTES,
  TASK_MAX_TASKS,
  TASK_NAME_MAX_BYTES,
  type ParsedTask,
} from "./taskBlob.ts";

export {
  parseTaskBlob,
  TaskDataError,
  TASK_BLOB_MARKER,
  TASK_BLOB_MAX_BYTES,
  TASK_BODY_MAX_BYTES,
  TASK_ID_MAX_BYTES,
  TASK_LINE_MAX_BYTES,
  TASK_MAX_TASKS,
  TASK_NAME_MAX_BYTES,
  TASK_PATH_MAX_BYTES,
  type ParsedTask,
  type TaskDataErrorCode,
} from "./taskBlob.ts";

const FROZEN_SCHEMA = "radxa-linkr-debugger.v1";

export interface TaskSummary {
  readonly id: string;
  readonly name: string;
  readonly request_count: number;
}

export interface TaskListData {
  readonly tasks: readonly TaskSummary[];
  readonly blob: string;
}

function asRecord(value: unknown): Record<string, unknown> | null {
  return typeof value === "object" && value !== null && !Array.isArray(value)
    ? Object.fromEntries(Object.entries(value))
    : null;
}

export function parseTaskListResponse(data: unknown): TaskListData {
  const envelope = asRecord(data);
  if (!envelope) throw new TaskDataError("invalid_response", "response is not a JSON object");
  if (envelope.schema !== FROZEN_SCHEMA) {
    throw new TaskDataError("invalid_response", `unexpected schema ${JSON.stringify(envelope.schema)}`);
  }
  if (envelope.ok !== true) throw new TaskDataError("invalid_response", "response does not report ok:true");
  if (envelope.command !== "task") {
    throw new TaskDataError("invalid_response", `unexpected command ${JSON.stringify(envelope.command)}`);
  }
  if (envelope.action !== "list") {
    throw new TaskDataError("invalid_response", `unexpected action ${JSON.stringify(envelope.action)}`);
  }
  if (!Array.isArray(envelope.tasks)) throw new TaskDataError("invalid_response", "tasks is not an array");
  if (envelope.tasks.length > TASK_MAX_TASKS) {
    throw new TaskDataError(
      "invalid_response",
      `tasks carries ${envelope.tasks.length} entries, above the firmware limit of ${TASK_MAX_TASKS}`,
    );
  }
  const tasks = envelope.tasks.map((entry): TaskSummary => {
    const summary = asRecord(entry);
    if (
      !summary ||
      typeof summary.id !== "string" ||
      typeof summary.name !== "string" ||
      typeof summary.request_count !== "number"
    ) {
      throw new TaskDataError("invalid_response", "task summary must carry id, name, request_count");
    }
    if (taskByteLength(summary.id) === 0 || taskByteLength(summary.id) > TASK_ID_MAX_BYTES) {
      throw new TaskDataError(
        "invalid_response",
        `task id must be 1..${TASK_ID_MAX_BYTES} UTF-8 bytes`,
      );
    }
    if (taskByteLength(summary.name) > TASK_NAME_MAX_BYTES) {
      throw new TaskDataError(
        "invalid_response",
        `task name exceeds the ${TASK_NAME_MAX_BYTES} byte firmware limit`,
      );
    }
    if (
      !Number.isInteger(summary.request_count) ||
      summary.request_count < 0 ||
      summary.request_count > TASK_MAX_REQUESTS
    ) {
      throw new TaskDataError(
        "invalid_response",
        `request_count must be an integer within 0..${TASK_MAX_REQUESTS}`,
      );
    }
    return { id: summary.id, name: summary.name, request_count: summary.request_count };
  });
  const seenIds = new Set<string>();
  for (const task of tasks) {
    if (seenIds.has(task.id)) {
      throw new TaskDataError("invalid_response", `task list carries duplicate id ${JSON.stringify(task.id)}`);
    }
    seenIds.add(task.id);
  }
  if (typeof envelope.blob !== "string") throw new TaskDataError("invalid_response", "blob is not a string");
  if (taskByteLength(envelope.blob) > TASK_BLOB_MAX_BYTES) {
    throw new TaskDataError(
      "invalid_response",
      `blob exceeds the ${TASK_BLOB_MAX_BYTES} byte firmware limit`,
    );
  }
  return { tasks, blob: envelope.blob };
}

export function selectTaskRequests(tasks: readonly ParsedTask[], taskId: string): readonly TaskRequest[] {
  const task = tasks.find((candidate) => candidate.id === taskId);
  if (!task) throw new TaskDataError("unknown_task", taskId);
  return task.requests;
}

export interface TaskRunProgress {
  readonly index: number;
  readonly total: number;
  readonly path: string;
}

export type TaskRunResult =
  | { readonly ok: true; readonly completed: number }
  | {
      readonly ok: false;
      readonly completed: number;
      readonly failedIndex: number;
      readonly failedPath: string;
      readonly error: string;
    }
  | {
      readonly ok: false;
      readonly completed: number;
      readonly attempted: number;
      readonly cancelled: true;
    };

export interface TaskRunDeps {
  readonly dispatch: (request: TaskRequest) => Promise<unknown>;
  readonly sleep: (ms: number, signal?: AbortSignal) => Promise<void>;
  readonly signal?: AbortSignal;
  readonly onProgress?: (progress: TaskRunProgress) => void;
}

export function sleepTaskDelay(ms: number, signal?: AbortSignal): Promise<void> {
  if (signal?.aborted) return Promise.resolve();
  return new Promise<void>((resolve) => {
    const finish = () => {
      clearTimeout(timer);
      signal?.removeEventListener("abort", finish);
      resolve();
    };
    const timer = setTimeout(finish, ms);
    signal?.addEventListener("abort", finish, { once: true });
  });
}

function waitInterruptibly(ms: number, deps: TaskRunDeps): Promise<boolean> {
  const signal = deps.signal;
  if (!signal) return deps.sleep(ms).then(() => false);
  if (signal.aborted) return Promise.resolve(true);
  return new Promise<boolean>((resolve) => {
    const onAbort = () => resolve(true);
    signal.addEventListener("abort", onAbort, { once: true });
    void deps.sleep(ms, signal).then(() => {
      signal.removeEventListener("abort", onAbort);
      resolve(false);
    });
  });
}

export async function runTaskRequests(
  requests: readonly TaskRequest[],
  deps: TaskRunDeps,
): Promise<TaskRunResult> {
  let attempted = 0;
  for (const [index, request] of requests.entries()) {
    if (deps.signal?.aborted) {
      return { ok: false, completed: index, attempted, cancelled: true };
    }
    deps.onProgress?.({ index: index + 1, total: requests.length, path: request.path });
    attempted = index + 1;
    try {
      await deps.dispatch(request);
    } catch (cause) {
      if (deps.signal?.aborted) {
        return { ok: false, completed: index, attempted, cancelled: true };
      }
      return {
        ok: false,
        completed: index,
        failedIndex: index + 1,
        failedPath: request.path,
        error: cause instanceof Error ? cause.message : String(cause),
      };
    }
    if (request.wait_ms !== undefined && request.wait_ms > 0) {
      if (await waitInterruptibly(request.wait_ms, deps)) {
        return { ok: false, completed: index + 1, attempted, cancelled: true };
      }
    }
  }
  return { ok: true, completed: requests.length };
}
