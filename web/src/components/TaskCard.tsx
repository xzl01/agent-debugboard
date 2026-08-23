import { useCallback, useEffect, useMemo, useState } from "react";
import { Loader2, RefreshCw, Workflow } from "lucide-react";
import { clearTasks, getTaskCatalog, getTasks, storeTaskBlob } from "@/lib/api";
import type { AutomationTaskControl } from "@/lib/automationTask";
import { mergeTaskCatalog, type BuiltInTask } from "@/lib/builtinTasks";
import type { TaskRunSnapshot } from "@/lib/taskExecution";
import { buildTaskBlob } from "@/lib/taskRequests";
import { TaskDataError, type TaskSummary } from "@/lib/taskRunner";
import { type TestScript } from "@/lib/testScript";
import { useI18n } from "@/lib/i18n";
import { Badge, Button, Card } from "./ui";
import { useTaskRun } from "./useTaskRun";

export function TaskCard({
  connected,
  currentScript,
  taskControl,
}: {
  connected: boolean;
  currentScript: TestScript;
  taskControl: AutomationTaskControl;
}) {
  const { t } = useI18n();
  const [tasks, setTasks] = useState<readonly TaskSummary[] | null>(null);
  const [builtIns, setBuiltIns] = useState<readonly BuiltInTask[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [catalogError, setCatalogError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [taskId, setTaskId] = useState("custom-task");

  const describeError = useCallback(
    (cause: unknown): string => {
      if (cause instanceof TaskDataError) {
        switch (cause.code) {
          case "invalid_response":
            return t("task.error.invalidResponse", { detail: cause.detail });
          case "invalid_blob":
            return t("task.error.invalidBlob", { detail: cause.detail });
          case "unknown_task":
            return t("task.error.unknownTask", { id: cause.detail });
          case "catalog_unavailable":
            return t("task.error.catalogUnavailable", { detail: cause.detail });
        }
      }
      return cause instanceof Error ? cause.message : String(cause);
    },
    [t],
  );

  const describeCatalogError = useCallback(
    (cause: unknown): string => {
      const detail = cause instanceof Error ? cause.message : String(cause);
      return t("task.error.catalogUnavailable", { detail });
    },
    [t],
  );

  // Stored tasks and the built-in catalog are independent firmware fetches:
  // a catalog failure only disables built-ins, never the stored task list.
  const refresh = useCallback(async () => {
    setError(null);
    setCatalogError(null);
    const [storedResult, catalogResult] = await Promise.allSettled([getTasks(), getTaskCatalog()]);
    if (storedResult.status === "fulfilled") {
      setTasks(storedResult.value.tasks);
    } else {
      setError(describeError(storedResult.reason));
    }
    if (catalogResult.status === "fulfilled") {
      setBuiltIns(catalogResult.value);
    } else {
      setBuiltIns(null);
      setCatalogError(describeCatalogError(catalogResult.reason));
    }
  }, [describeError, describeCatalogError]);

  useEffect(() => {
    if (connected) void refresh();
  }, [connected, refresh]);

  const confirmRun = useCallback(
    (snapshot: TaskRunSnapshot): boolean =>
      window.confirm(
        t("task.run.confirm", {
          id: snapshot.id,
          source: t(snapshot.source === "builtin" ? "task.source.builtin" : "task.source.stored"),
          count: snapshot.requests.length,
        }),
      ),
    [t],
  );

  const taskBusyMessage = useCallback(() => t("task.error.taskBusy"), [t]);

  const { progress, outcome, runTask, cancelTask } = useTaskRun({
    setBusy,
    setError,
    setTasks,
    describeError,
    taskControl,
    confirmRun,
    taskBusyMessage,
  });

  const catalog = useMemo(
    () => mergeTaskCatalog(builtIns, tasks ?? []),
    [builtIns, tasks],
  );
  const storedCount = tasks?.length ?? 0;

  const runBusy = async (action: () => Promise<unknown>) => {
    setBusy(true);
    setError(null);
    try {
      await action();
      await refresh();
    } catch (cause) {
      setError(describeError(cause));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Card
      title={t("task.title")}
      subtitle={t("task.subtitle")}
      icon={Workflow}
      right={
        <Button
          variant="ghost"
          className="px-2 py-1"
          onClick={() => void refresh()}
          disabled={busy || !connected}
          aria-label={t("task.refresh")}
        >
          <RefreshCw size={15} className={busy ? "animate-spin" : undefined} />
        </Button>
      }
    >
      <section className="rounded-xl border border-line/60 bg-panel2/35 p-3">
        <div className="text-sm font-medium text-ink">{t("task.customTitle")}</div>
        <p className="mt-1 text-xs text-ink-dim">{t("task.customSubtitle")}</p>
        <label className="mt-3 grid gap-1.5 text-xs text-ink-dim">
          {t("task.taskId")}
          <input
            value={taskId}
            onChange={(event) => setTaskId(event.target.value)}
            disabled={busy}
            className="min-h-10 rounded-xl border border-line/70 bg-panel2 px-3 text-sm text-ink outline-none focus-visible:ring-2 focus-visible:ring-brand/40"
          />
        </label>
        <div className="mt-3 flex flex-wrap gap-2">
          <Button
            variant="default"
            disabled={busy || !connected || taskId.trim().length === 0}
            onClick={() =>
              void runBusy(() =>
                storeTaskBlob(buildTaskBlob(taskId.trim(), currentScript)),
              )
            }
          >
            {busy && <Loader2 size={15} className="animate-spin" />}
            {t("task.storeCurrent")}
          </Button>
        </div>
      </section>

      <div className="mt-4 space-y-2">
        {tasks !== null && storedCount === 0 && (
          <p className="text-xs text-ink-dim">{t("task.none")}</p>
        )}
        {catalog.map((task) => {
          const running = progress?.taskId === task.id;
          return (
            <div key={task.id} className="rounded-lg border border-line/50 bg-panel2/40 px-3 py-2">
              <div className="flex flex-wrap items-center justify-between gap-2">
                <div className="min-w-0">
                  <div className="flex flex-wrap items-center gap-2">
                    <span className="text-sm font-medium text-ink">{task.name}</span>
                    <Badge tone={task.source === "builtin" ? "brand" : "neutral"}>
                      {task.source === "builtin" ? t("task.source.builtin") : t("task.source.stored")}
                    </Badge>
                    {task.source === "builtin" && task.shadowedStored && (
                      <Badge tone="warn">{t("task.shadowedStored")}</Badge>
                    )}
                    {running && progress && (
                      <span className="text-xs text-ink-dim">
                        {t("task.running", { index: progress.index, total: progress.total })}
                      </span>
                    )}
                  </div>
                  <div className="text-xs text-ink-dim break-all font-mono">{task.id} · {t("task.requestCount", { count: task.requestCount })}</div>
                </div>
                <div className="flex gap-1.5">
                  {running ? (
                    <Button
                      variant="ghost"
                      className="px-2 py-1 text-xs"
                      onClick={cancelTask}
                    >
                      {t("task.cancel")}
                    </Button>
                  ) : (
                    <Button
                      variant="ghost"
                      className="px-2 py-1 text-xs"
                      disabled={busy || !connected}
                      onClick={() => void runTask(task.id, builtIns)}
                    >
                      {t("task.run")}
                    </Button>
                  )}
                </div>
              </div>
            </div>
          );
        })}
        {outcome && (
          <div className={`rounded-lg border px-3 py-2 text-xs break-all ${outcome.ok ? "border-ok/40 bg-ok/10 text-ok" : outcome.cancelled ? "border-warn/40 bg-warn/10 text-warn" : "border-danger/40 bg-danger/10 text-danger"}`}>
            {outcome.ok
              ? t("task.runOk", { count: outcome.completed })
              : outcome.cancelled
                ? t("task.runCancelled", { completed: outcome.completed, total: outcome.total })
                : `${t("task.runFailed", { index: outcome.failedIndex ?? "?", total: outcome.total })} ${t("task.runFailedDetail", { path: outcome.failedPath ?? "?", error: outcome.error ?? "?" })}`}
            {outcome.cleanupError ? ` ${t("task.runCleanupFailed", { error: outcome.cleanupError })}` : ""}
          </div>
        )}
      </div>

      {error && <p className="mt-3 text-xs text-danger">{error}</p>}
      {catalogError && <p className="mt-3 text-xs text-warn">{catalogError}</p>}

      {tasks !== null && storedCount > 0 && (
        <Button variant="ghost" className="mt-3 w-full" disabled={busy} onClick={() => void runBusy(() => clearTasks())}>
          {t("task.clearTasks")}
        </Button>
      )}
    </Card>
  );
}
