import { useEffect, useRef, useState } from "react";
import { dispatchTaskRequest, getTasks } from "@/lib/api";
import type { AutomationTaskControl } from "@/lib/automationTask";
import type { BuiltInTask } from "@/lib/builtinTasks";
import {
  executeTaskRun,
  resolveTaskRunSnapshot,
  type TaskExecutionOutcome,
  type TaskRunSnapshot,
} from "@/lib/taskExecution";
import { sleepTaskDelay, type TaskSummary } from "@/lib/taskRunner";

export interface RunProgress {
  readonly taskId: string;
  readonly index: number;
  readonly total: number;
}

export interface RunOutcome {
  readonly taskId: string;
  readonly ok: boolean;
  readonly completed: number;
  readonly total: number;
  readonly failedIndex?: number;
  readonly failedPath?: string;
  readonly error?: string;
  readonly cancelled?: boolean;
  readonly cleanupError?: string;
}

function toOutcome(taskId: string, total: number, result: TaskExecutionOutcome): RunOutcome {
  const cleanupError = result.kind === "success" || result.cleanup == null || result.cleanup.ok
    ? {}
    : { cleanupError: result.cleanup.error ?? "cleanup failed" };
  switch (result.kind) {
    case "success":
      return { taskId, ok: true, completed: result.completed, total };
    case "failed":
      return {
        taskId,
        ok: false,
        completed: result.completed,
        total,
        failedIndex: result.failedIndex,
        failedPath: result.failedPath,
        error: result.error,
        ...cleanupError,
      };
    case "cancelled":
      return { taskId, ok: false, cancelled: true, completed: result.completed, total, ...cleanupError };
  }
}

export function useTaskRun({
  setBusy,
  setError,
  setTasks,
  describeError,
  taskControl,
  confirmRun,
  taskBusyMessage,
}: {
  readonly setBusy: (busy: boolean) => void;
  readonly setError: (message: string | null) => void;
  readonly setTasks: (tasks: readonly TaskSummary[]) => void;
  readonly describeError: (cause: unknown) => string;
  readonly taskControl: AutomationTaskControl;
  readonly confirmRun: (snapshot: TaskRunSnapshot) => boolean;
  readonly taskBusyMessage: () => string;
}) {
  const [progress, setProgress] = useState<RunProgress | null>(null);
  const [outcome, setOutcome] = useState<RunOutcome | null>(null);
  const mountedRef = useRef(true);
  const abortRef = useRef<AbortController | null>(null);
  const runningRef = useRef(false);

  useEffect(() => {
    mountedRef.current = true;
    return () => {
      mountedRef.current = false;
      abortRef.current?.abort();
    };
  }, []);

  const runTask = async (id: string, catalog: readonly BuiltInTask[] | null) => {
    if (runningRef.current) return;
    runningRef.current = true;
    setBusy(true);
    setError(null);
    setOutcome(null);
    const controller = new AbortController();
    abortRef.current = controller;
    let lockHeld = false;
    try {
      // Resolve the execution snapshot first; confirmation binds to these
      // exact in-memory requests and execution reuses them without refetching.
      const resolved = await resolveTaskRunSnapshot(id, catalog, getTasks, controller.signal);
      if (!mountedRef.current || controller.signal.aborted) return;
      if (resolved.storedTasks) setTasks(resolved.storedTasks);
      if (!confirmRun(resolved.snapshot)) return;
      if (!taskControl.acquire("task")) {
        setError(taskBusyMessage());
        return;
      }
      lockHeld = true;
      const { snapshot } = resolved;
      const result = await executeTaskRun(snapshot, {
        dispatch: (request, signal) => dispatchTaskRequest(request.path, request.body, signal),
        sleep: sleepTaskDelay,
        signal: controller.signal,
        onProgress: (p) => {
          if (mountedRef.current) setProgress({ taskId: id, index: p.index, total: p.total });
        },
      });
      if (mountedRef.current) setOutcome(toOutcome(id, snapshot.requests.length, result));
    } catch (cause) {
      if (mountedRef.current && !controller.signal.aborted) setError(describeError(cause));
    } finally {
      abortRef.current = null;
      runningRef.current = false;
      if (lockHeld) taskControl.release("task");
      if (mountedRef.current) {
        setBusy(false);
        setProgress(null);
      }
    }
  };

  const cancelTask = () => {
    abortRef.current?.abort();
  };

  return { progress, outcome, runTask, cancelTask };
}
