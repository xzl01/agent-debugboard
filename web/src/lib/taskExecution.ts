import { findBuiltInTask, BUILTIN_TASK_ID_PREFIX, type BuiltInTask } from "./builtinTasks.ts";
import type { TaskRequest } from "./taskRequests.ts";
import {
  parseTaskBlob,
  runTaskRequests,
  selectTaskRequests,
  TaskDataError,
  type TaskListData,
  type TaskRunProgress,
  type TaskSummary,
} from "./taskRunner.ts";

/**
 * Execution snapshot resolved and confirmed before any control request is
 * dispatched. Built-ins resolve from the firmware catalog fetched at card
 * refresh time and carry typed cleanup metadata; stored tasks resolve through
 * the firmware list + strict blob parse and never infer cleanup.
 */
export interface TaskRunSnapshot {
  readonly id: string;
  readonly name: string;
  readonly source: "builtin" | "stored";
  readonly requests: readonly TaskRequest[];
  readonly cleanup: TaskRequest | null;
}

export interface ResolvedTaskRun {
  readonly snapshot: TaskRunSnapshot;
  readonly storedTasks?: readonly TaskSummary[];
}

export async function resolveTaskRunSnapshot(
  id: string,
  catalog: readonly BuiltInTask[] | null,
  fetchTasks: (signal?: AbortSignal) => Promise<TaskListData>,
  signal?: AbortSignal,
): Promise<ResolvedTaskRun> {
  // `null` catalog means the firmware catalog fetch/parse failed; built-ins
  // are unavailable while stored tasks stay independently resolvable.
  const builtIn = catalog !== null ? findBuiltInTask(catalog, id) : undefined;
  if (builtIn) {
    return {
      snapshot: {
        id: builtIn.id,
        name: builtIn.name,
        source: "builtin",
        requests: builtIn.requests,
        cleanup: builtIn.cleanup,
      },
    };
  }
  // The builtin/ namespace is firmware-owned and immutable: fail closed
  // before any stored fetch so a stored blob can never impersonate a recipe.
  if (id.startsWith(BUILTIN_TASK_ID_PREFIX)) {
    throw new TaskDataError("catalog_unavailable", id);
  }
  const data = await fetchTasks(signal);
  const requests = selectTaskRequests(parseTaskBlob(data.blob), id);
  return {
    snapshot: { id, name: data.tasks.find((task) => task.id === id)?.name ?? id, source: "stored", requests, cleanup: null },
    storedTasks: data.tasks,
  };
}

export interface CleanupOutcome {
  readonly attempted: boolean;
  readonly ok: boolean;
  readonly error?: string;
}

export type TaskExecutionOutcome =
  | { readonly kind: "success"; readonly completed: number }
  | {
      readonly kind: "failed";
      readonly completed: number;
      readonly failedIndex: number;
      readonly failedPath: string;
      readonly error: string;
      readonly cleanup: CleanupOutcome | null;
    }
  | { readonly kind: "cancelled"; readonly completed: number; readonly cleanup: CleanupOutcome | null };

export interface TaskExecutionDeps {
  readonly dispatch: (request: TaskRequest, signal?: AbortSignal) => Promise<unknown>;
  readonly sleep: (ms: number) => Promise<void>;
  readonly signal?: AbortSignal;
  readonly onProgress?: (progress: TaskRunProgress) => void;
}

const CLEANUP_TIMEOUT_MS = 5000;

async function attemptCleanup(
  cleanup: TaskRequest,
  dispatch: TaskExecutionDeps["dispatch"],
): Promise<CleanupOutcome> {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), CLEANUP_TIMEOUT_MS);
  try {
    await dispatch(cleanup, controller.signal);
    return { attempted: true, ok: true };
  } catch (cause) {
    return { attempted: true, ok: false, error: cause instanceof Error ? cause.message : String(cause) };
  } finally {
    clearTimeout(timeout);
  }
}

export async function executeTaskRun(
  snapshot: TaskRunSnapshot,
  deps: TaskExecutionDeps,
): Promise<TaskExecutionOutcome> {
  const result = await runTaskRequests(snapshot.requests, {
    dispatch: (request) => deps.dispatch(request, deps.signal),
    sleep: deps.sleep,
    signal: deps.signal,
    onProgress: deps.onProgress,
  });
  if (result.ok) return { kind: "success", completed: result.completed };
  const attempted = "cancelled" in result ? result.attempted : result.failedIndex;
  const cleanup = snapshot.cleanup != null && attempted > 0
    ? await attemptCleanup(snapshot.cleanup, deps.dispatch)
    : null;
  if ("cancelled" in result) return { kind: "cancelled", completed: result.completed, cleanup };
  return {
    kind: "failed",
    completed: result.completed,
    failedIndex: result.failedIndex,
    failedPath: result.failedPath,
    error: result.error,
    cleanup,
  };
}
