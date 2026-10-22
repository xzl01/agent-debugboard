import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Loader2, RefreshCw, Workflow } from "lucide-react";
import { clearTasks, deleteTask, getTaskCatalog, getTaskMetadata, getTaskIr, getTaskSource, upsertWorkflowTask, mutateTasks, importTaskIr } from "@/lib/api";
import type { AutomationTaskControl } from "@/lib/automationTask";
import { mergeTaskCatalog, type BuiltInTask } from "@/lib/builtinTasks";
import type { TaskRunSnapshot } from "@/lib/taskExecution";
import { serializeTaskRequests } from "@/lib/taskRequests";
import { TaskDataError, type TaskSummary } from "@/lib/taskRunner";
import { parseTestScript, serializeTestScript, type TestScript } from "@/lib/testScript";
import { parseTaskBlob, taskByteLength } from "@/lib/taskBlob";
import { taskIrSection, TaskV2Error, TASK_SOURCE_MAX_BYTES, type TaskStoreMetadata } from "@/lib/taskV2";
import { workflowLibrary } from "@/lib/workflowLibrary";
import { downloadBlob } from "@/lib/utils";
import { useI18n } from "@/lib/i18n";
import { Badge, Button, Card } from "./ui";
import { useTaskRun } from "./useTaskRun";
import { SourceImportDialog } from "./SourceImportDialog";

function kib(bytes: number): string {
  return `${(bytes / 1024).toFixed(1)} KiB`;
}

export function TaskCard({
  connected,
  currentScript,
  taskControl,
  workflowId,
  onOpenWorkflow,
  onLibraryChange,
}: {
  connected: boolean;
  currentScript: TestScript;
  taskControl: AutomationTaskControl;
  workflowId?: string | null;
  onOpenWorkflow?: (script: TestScript) => void;
  onLibraryChange?: () => void;
}) {
  const { t } = useI18n();
  const [tasks, setTasks] = useState<readonly TaskSummary[] | null>(null);
  const [builtIns, setBuiltIns] = useState<readonly BuiltInTask[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [catalogError, setCatalogError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [taskId, setTaskId] = useState("custom-task");
  const [metadata, setMetadata] = useState<TaskStoreMetadata | null>(null);
  const storageBlocked = metadata?.storage?.measured === true && !metadata.storage.save_allowed;
  const [importOpen, setImportOpen] = useState(false);
  const [notice, setNotice] = useState<string | null>(null);
  const [unavailableSources, setUnavailableSources] = useState<ReadonlySet<string>>(new Set());
  const [localSourceIds, setLocalSourceIds] = useState<ReadonlySet<string>>(new Set());
  const refreshGeneration = useRef(0);
  const openWorkflowRef = useRef(onOpenWorkflow);
  openWorkflowRef.current = onOpenWorkflow;
  const localOnly = useMemo(() => {
    try { return taskByteLength(serializeTestScript(currentScript)) > TASK_SOURCE_MAX_BYTES; }
    catch { return false; } // Invalid editing buffers remain renderable; strict deployment reports the error below.
  }, [currentScript]);
  const validationError = useMemo(() => {
    try { parseTestScript(serializeTestScript(currentScript)); return null; }
    catch (cause) { return `${t("library.validationError")} ${cause instanceof Error ? cause.message : String(cause)}`; }
  }, [currentScript, t]);

  const describeError = useCallback(
    (cause: unknown): string => {
      if (cause instanceof TaskV2Error) {
        if (cause.code === "storage_full") return t("task.v2.storageFull");
        return `${t(cause.code === "revision_conflict" || cause.code === "catalog_conflict" ? "task.v2.conflict" : "task.v2.failed")} ${cause.message}`;
      }
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
    const generation = ++refreshGeneration.current;
    setError(null);
    setCatalogError(null);
    setUnavailableSources(new Set());
    setLocalSourceIds(new Set());
    const [storedResult, catalogResult] = await Promise.allSettled([getTaskMetadata(), getTaskCatalog()]);
    if (generation !== refreshGeneration.current) return;
    if (storedResult.status === "fulfilled") {
      setMetadata(storedResult.value);
      setTasks(storedResult.value.tasks.map((task) => ({ ...task, name: task.id })));
    } else {
      setMetadata(null);
      setError(describeError(storedResult.reason));
    }
    if (catalogResult.status === "fulfilled") {
      setBuiltIns(catalogResult.value);
    } else {
      setBuiltIns(null);
      setCatalogError(describeCatalogError(catalogResult.reason));
    }
    if (storedResult.status === "fulfilled") {
      const metadata = storedResult.value;
      const lookups = await Promise.all(metadata.tasks.filter((task) => task.source_state === "absent" && !task.id.startsWith("builtin/")).map(async (task) => {
        try { return { id: task.id, local: (await workflowLibrary.openDeployment(metadata, task.id))?.local_only === true, failed: false }; }
        catch { return { id: task.id, local: false, failed: true }; }
      }));
      if (generation !== refreshGeneration.current) return;
      setLocalSourceIds(new Set(lookups.filter((lookup) => lookup.local).map((lookup) => lookup.id)));
      setUnavailableSources(new Set(lookups.filter((lookup) => lookup.failed).map((lookup) => lookup.id)));
    }
  }, [describeError, describeCatalogError]);

  useEffect(() => {
    if (connected) void refresh();
    return () => { refreshGeneration.current += 1; };
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

  const runBusy = async (action: () => Promise<unknown>, rethrow = false) => {
    if (busy || !connected || !taskControl.acquire("task")) {
      const cause = new Error(t("task.error.taskBusy"));
      setError(cause.message); if (rethrow) throw cause; return;
    }
    setBusy(true);
    setError(null);
    setNotice(null);
    try {
      await action();
      await refresh();
    } catch (cause) {
      setError(describeError(cause));
      if (rethrow) throw new Error(describeError(cause));
    } finally {
      setBusy(false);
      taskControl.release("task");
    }
  };

  const handleDelete = (id: string) => {
    if (!window.confirm(t("task.delete.confirm", { id }))) return;
    if (!metadata) return;
    void runBusy(() => deleteTask(id, metadata));
  };

  const storeCurrent = () => {
    if (!metadata) return;
    if (storageBlocked) { setError(t("task.v2.storageFull")); return; }
    if (validationError) { setError(validationError); return; }
    const id = taskId.trim();
    if (id.startsWith("builtin/")) { setError(t("task.v2.reserved")); return; }
    if (metadata.tasks.some((task) => task.id === id) && !window.confirm(t("task.v2.overwrite", { id }))) return;
    void runBusy(async () => {
      const deployed = await upsertWorkflowTask(metadata, id, currentScript);
      setNotice(t(deployed.local_only ? "task.v2.deployedIr" : "task.v2.deployed"));
      // A deployment is committed already; a local quota error must not claim the board write failed.
      try {
        const existing = workflowId ? await workflowLibrary.getWorkflow(workflowId) : null;
        const workflow = existing?.source === deployed.source ? existing : await workflowLibrary.saveWorkflow({ source: deployed.source, baseline: deployed.source });
        await workflowLibrary.saveDeployment({ task_store_id: deployed.result.task_store_id, task_id: id, workflow_id: workflow.workflow_id,
          source_digest: deployed.source_digest, task_ir_digest: deployed.task_ir_digest, catalog_fingerprint: deployed.result.catalog_fingerprint,
          revision: deployed.result.revision, source_state: deployed.local_only ? "local-only" : "board" });
        onLibraryChange?.();
      } catch (cause) { setNotice(`${t("task.v2.localSaveFailed")} ${cause instanceof Error ? cause.message : String(cause)}`); }
    });
  };
  const exportIr = async (id?: string) => {
    setBusy(true); setError(null);
    try {
      const builtin = builtIns?.find((task) => task.id === id);
      if (builtin && !window.confirm(t("task.v2.builtinExport"))) return;
      const blob = builtin ? `# linkr-task.v1\n# task ${builtin.id}\n${serializeTaskRequests(builtin.requests)}` : (await getTaskIr()).blob;
      downloadBlob(`${(id ?? "all-tasks").replaceAll("/", "-")}.ndjson`, id ? taskIrSection(blob, id) : blob, "application/x-ndjson");
    } catch (cause) { setError(describeError(cause)); }
    finally { setBusy(false); }
  };
  const openSource = async (id: string) => {
    setBusy(true); setError(null);
    try {
      const fresh = await getTaskMetadata();
      const task = fresh.tasks.find((task) => task.id === id);
      if (task?.source_state === "present") {
        const source = await getTaskSource(id);
        openWorkflowRef.current?.(source.script);
      } else {
        const local = task && await workflowLibrary.openDeployment(fresh, id);
        if (!local?.local_only) throw new Error(t("task.v2.localSourceUnavailable"));
        openWorkflowRef.current?.(parseTestScript(local.source));
      }
    }
    catch (cause) {
      setLocalSourceIds((previous) => new Set([...previous].filter((candidate) => candidate !== id)));
      setUnavailableSources((previous) => new Set([...previous, id]));
      setError(describeError(cause));
    }
    finally { setBusy(false); }
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
        {localOnly && <p role="status" className="mt-2 text-xs text-warn">{t("task.v2.localOnly")}</p>}
        {validationError && <p role="status" className="mt-2 break-words text-xs text-warn">{validationError}</p>}
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
            disabled={busy || !connected || !metadata || storageBlocked || taskId.trim().length === 0}
            onClick={storeCurrent}
          >
            {busy && <Loader2 size={15} className="animate-spin" />}
            {t("task.storeCurrent")}
          </Button>
        </div>
      </section>

      <section aria-label={t("task.v2.irTools")} className="mt-3 flex min-w-0 flex-wrap items-center gap-2">
        <Button disabled={busy || !connected || !metadata || storageBlocked} onClick={() => setImportOpen(true)}>{t("task.v2.importIr")}</Button>
        <Button disabled={busy || !connected} onClick={() => void exportIr()}>{t("task.v2.exportAllIr")}</Button>
        {metadata && <span className="break-all text-[11px] text-ink-dim">{t("task.v2.capacity", { count: metadata.tasks.length, bytes: metadata.full_ir_bytes })} · r{metadata.revision}</span>}
        {metadata?.storage?.measured && <span className="break-all text-[11px] text-ink-dim">{t("task.v2.storage", { used: kib(metadata.storage.used_bytes), free: kib(metadata.storage.free_bytes), task: kib(metadata.storage.task_bytes) })}</span>}
      </section>
      {storageBlocked && <p role="alert" className="mt-2 text-xs text-danger">{t("task.v2.storageBlocked", { reserve: kib(metadata?.storage?.reserve_bytes ?? 0) })}</p>}
      <p className="mt-2 text-[11px] text-ink-dim">{t("task.v2.irHint")}</p>
      {notice && <p role="status" className="mt-3 break-words text-xs text-ink">{notice}</p>}

      <div className="mt-4 space-y-2">
        {tasks !== null && storedCount === 0 && (
          <p className="text-xs text-ink-dim">{t("task.none")}</p>
        )}
        {catalog.map((task) => {
          const running = progress?.taskId === task.id;
          const boardSource = task.source === "stored" && metadata?.tasks.find((entry) => entry.id === task.id)?.source_state === "present";
          const localSource = task.source === "stored" && !boardSource && localSourceIds.has(task.id);
          return (
            <div key={task.id} className="rounded-lg border border-line/50 bg-panel2/40 px-3 py-2">
              <div className="flex flex-wrap items-center justify-between gap-2">
                <div className="min-w-0">
                  <div className="flex flex-wrap items-center gap-2">
                    <span className="min-w-0 break-all text-sm font-medium text-ink">{task.name}</span>
                    <Badge tone={task.source === "builtin" ? "brand" : "neutral"}>
                      {task.source === "builtin" ? t("task.source.builtin") : t("task.source.stored")}
                    </Badge>
                    <Badge tone={unavailableSources.has(task.id) || localSource ? "warn" : "neutral"}>{t(task.source === "builtin" ? "task.v2.builtinSource" : unavailableSources.has(task.id) ? "task.v2.sourceInvalid" : boardSource ? "task.v2.lossless" : localSource ? "task.v2.localSource" : "task.v2.irOnly")}</Badge>
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
                <div className="flex flex-wrap gap-1.5">
                  {(boardSource || localSource) && <Button variant="ghost" className="px-2 py-1 text-xs" disabled={busy || !connected || !onOpenWorkflow || unavailableSources.has(task.id)} onClick={() => void openSource(task.id)}>{t(localSource ? "task.v2.openLocalSource" : "task.v2.openSource")}</Button>}
                  <Button variant="ghost" className="px-2 py-1 text-xs" disabled={busy || !connected} onClick={() => void exportIr(task.id)}>{t("task.v2.exportIr")}</Button>
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
                  {(task.source === "stored" || task.shadowedStored) && !running && (
                    <Button
                      variant="ghost"
                      className="px-2 py-1 text-xs text-danger"
                      disabled={busy || !connected}
                      onClick={() => handleDelete(task.id)}
                    >
                      {t("task.delete")}
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

      {error && <p role="alert" className="mt-3 break-words text-xs text-danger">{error}</p>}
      {catalogError && <p className="mt-3 text-xs text-warn">{catalogError}</p>}

      {tasks !== null && storedCount > 0 && (
        <Button variant="ghost" className="mt-3 w-full" disabled={busy || !connected || !metadata} onClick={() => { if (metadata && window.confirm(t("task.v2.clearConfirm"))) void runBusy(() => clearTasks(metadata)); }}>
          {t("task.clearTasks")}
        </Button>
      )}
      {importOpen && <SourceImportDialog title={t("task.v2.importIr")} description={t("task.v2.irHint")} onClose={() => setImportOpen(false)} allowAll parse={(text) => parseTaskBlob(text).map((task) => ({ id: task.id, label: task.id, detail: t("task.requestCount", { count: task.requests.length }), value: task }))} onImport={async (entry, text) => {
        if (!metadata) throw new Error(t("task.v2.refreshFirst"));
        if (parseTaskBlob(text).some((task) => task.id.startsWith("builtin/"))) throw new Error(t("task.v2.reserved"));
        if (!entry && !window.confirm(t("task.v2.replaceConfirm"))) throw new Error(t("task.v2.cancelled"));
        if (entry && metadata.tasks.some((task) => task.id === entry.id) && !window.confirm(t("task.v2.overwrite", { id: entry.id }))) throw new Error(t("task.v2.cancelled"));
        await runBusy(() => entry ? mutateTasks({ action: "upsert", id: entry.id, ir: taskIrSection(text, entry.id), expectedRevision: metadata.revision, catalogFingerprint: metadata.catalog_fingerprint }) : importTaskIr(text, metadata), true);
      }} />}
    </Card>
  );
}
