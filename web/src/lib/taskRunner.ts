import { type TaskRequest } from "./taskRequests.ts";
import { TaskDataError, type ParsedTask } from "./taskBlob.ts";
export { parseTaskStoreMetadata as parseTaskListResponse } from "./taskV2.ts";

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

export interface TaskSummary {
  readonly id: string;
  readonly name: string;
  readonly request_count: number;
}

export interface TaskListData {
  readonly tasks: readonly TaskSummary[];
  readonly blob: string;
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
