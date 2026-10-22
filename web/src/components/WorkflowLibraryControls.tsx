import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { FolderOpen, Plus, Save, Trash2 } from "lucide-react";
import { workflowLibrary, WorkflowStorageError, type WorkflowRecord } from "@/lib/workflowLibrary";
import { defaultScript, parseTestScript, parseTestScriptDraft, serializeTestScript, serializeTestScriptDraft, type TestScript } from "@/lib/testScript";
import { useI18n } from "@/lib/i18n";
import { Badge, Button, Dialog } from "./ui";

const DRAFT_KEY = "linkr-test-script";
const IDENTITY_KEY = "linkr-workflow-editor";

function restoreDraft(name: string) {
  try {
    const source = localStorage.getItem(DRAFT_KEY);
    const script = source ? parseTestScriptDraft(source) : defaultScript(name);
    const identity = JSON.parse(localStorage.getItem(IDENTITY_KEY) ?? "null");
    return { script, id: identity?.source === source && typeof identity.id === "string" ? identity.id as string : null,
      baseline: identity?.source === source && typeof identity.baseline === "string" ? identity.baseline as string : serializeTestScriptDraft(script) };
  } catch { const script = defaultScript(name); return { script, id: null, baseline: serializeTestScriptDraft(script) }; }
}

/** The editing buffer is independent of the explicitly saved IndexedDB library. */
export function useWorkflowEditor(defaultName: string) {
  const { t } = useI18n();
  const [initial] = useState(() => restoreDraft(defaultName));
  const [script, setScript] = useState(initial.script);
  const [activeId, setActiveId] = useState<string | null>(initial.id);
  const [baseline, setBaseline] = useState(initial.baseline);
  const [documentEpoch, setDocumentEpoch] = useState(0);
  const [workflows, setWorkflows] = useState<readonly WorkflowRecord[]>([]);
  const [draftState, setDraftState] = useState<"saving" | "saved" | "error">("saving");
  const [error, setError] = useState<string | null>(null);
  const [draftError, setDraftError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [pending, setPending] = useState<(() => void) | null>(null);
  const [saveAsName, setSaveAsName] = useState<string | null>(null);
  const [continueAfterSave, setContinueAfterSave] = useState(false);
  const draft = useMemo(() => {
    try { return { source: serializeTestScriptDraft(script), error: null }; }
    catch (cause) { return { source: null, error: `${t("library.draftError")} ${cause instanceof Error ? cause.message : String(cause)}` }; }
  }, [script, t]);
  const source = draft.source;
  const validationError = useMemo(() => {
    try { parseTestScript(serializeTestScript(script)); return null; }
    catch (cause) { return `${t("library.validationError")} ${cause instanceof Error ? cause.message : String(cause)}`; }
  }, [script, t]);
  const generationRef = useRef(0);
  const openRequestRef = useRef(0);
  const currentSourceRef = useRef(source);
  currentSourceRef.current = source;
  const savingRef = useRef(false);
  const dirty = source === null || source !== baseline;

  const describeError = useCallback((cause: unknown) => {
    const key = cause instanceof WorkflowStorageError ? cause.code : "storage_error";
    setError(`${t(`library.error.${key}`)} ${cause instanceof Error ? cause.message : String(cause)}`);
  }, [t]);
  const refresh = useCallback(async () => {
    try { setWorkflows(await workflowLibrary.listWorkflows()); setError(null); }
    catch (cause) { describeError(cause); }
  }, [describeError]);
  useEffect(() => { void refresh(); }, []);
  useEffect(() => () => { openRequestRef.current += 1; }, []);
  useEffect(() => {
    if (source === null) { setDraftState("error"); setDraftError(draft.error); return; }
    try {
      localStorage.setItem(DRAFT_KEY, source);
      localStorage.setItem(IDENTITY_KEY, JSON.stringify({ id: activeId, baseline, source }));
      setDraftState("saved"); setDraftError(null);
    } catch (cause) {
      setDraftState("error"); setDraftError(`${t("library.draftError")} ${String(cause)}`);
    }
  }, [source, draft.error, activeId, baseline, t]);

  const change = useCallback((next: TestScript) => { generationRef.current += 1; setDraftState("saving"); setScript(next); }, []);
  const replace = useCallback((next: TestScript, id: string | null = null, savedBaseline?: string, resetHistory = true) => {
    generationRef.current += 1;
    setScript(next); setActiveId(id); setBaseline(savedBaseline ?? serializeTestScriptDraft(next));
    if (resetHistory) setDocumentEpoch((value) => value + 1);
  }, []);
  const guard = useCallback((action: () => void) => {
    if (dirty) setPending(() => action); else action();
  }, [dirty]);
  const save = async (asName?: string): Promise<boolean> => {
    if (busy || savingRef.current) return false;
    savingRef.current = true;
    setBusy(true); setError(null);
    try {
      const next = asName === undefined ? script : { ...script, name: asName.trim() };
      let savedSource: string;
      try { savedSource = serializeTestScript(parseTestScript(serializeTestScript(next))); }
      catch (cause) { setError(`${t("library.validationError")} ${cause instanceof Error ? cause.message : String(cause)}`); return false; }
      const record = await workflowLibrary.saveWorkflow({ ...(asName === undefined && activeId ? { workflow_id: activeId } : {}), source: savedSource, baseline: savedSource });
      const savedScript = parseTestScript(record.source);
      const savedDraft = serializeTestScriptDraft(savedScript);
      if (currentSourceRef.current === source) replace(savedScript, record.workflow_id, savedDraft, asName !== undefined);
      else { setActiveId(record.workflow_id); setBaseline(savedDraft); }
      setWorkflows((previous) => [...previous.filter((row) => row.workflow_id !== record.workflow_id), record]);
      return true;
    } catch (cause) { describeError(cause); return false; }
    finally { savingRef.current = false; setBusy(false); }
  };
  const open = (record: WorkflowRecord) => guard(() => {
    // Always read and validate the current record, not a stale list-row snapshot.
    const generation = generationRef.current;
    const request = ++openRequestRef.current;
    setBusy(true); setError(null);
    void workflowLibrary.getWorkflow(record.workflow_id).then((fresh) => {
      if (request !== openRequestRef.current) return;
      if (generation !== generationRef.current) { setError(t("library.openCancelled")); return; }
      if (!fresh) throw new Error(t("library.missing"));
      replace(parseTestScript(fresh.source), fresh.workflow_id, serializeTestScriptDraft(parseTestScript(fresh.baseline ?? fresh.source)));
    }).catch((cause) => { if (request === openRequestRef.current) describeError(cause); }).finally(() => { if (request === openRequestRef.current) setBusy(false); });
  });
  const remove = async (record: WorkflowRecord) => {
    if (!window.confirm(t("library.deleteConfirm", { name: record.name }))) return;
    // Deleting an entry never throws away the open editing buffer.
    setBusy(true);
    try {
      await workflowLibrary.deleteWorkflow(record.workflow_id);
      if (activeId === record.workflow_id) { setActiveId(null); setBaseline(""); }
      setWorkflows((previous) => previous.filter((row) => row.workflow_id !== record.workflow_id));
    } catch (cause) { describeError(cause); }
    finally { setBusy(false); }
  };
  const cancelPending = useCallback(() => { if (!busy) { setPending(null); setContinueAfterSave(false); } }, [busy]);
  const cancelSaveAs = useCallback(() => { if (!busy) { setSaveAsName(null); setContinueAfterSave(false); } }, [busy]);
  const proceed = () => { const action = pending; setPending(null); action?.(); };
  const saveAndProceed = async () => { if (await save()) proceed(); };
  const saveAsAndProceed = async () => {
    if (!saveAsName?.trim()) return;
    if (await save(saveAsName)) { setSaveAsName(null); if (continueAfterSave) proceed(); setContinueAfterSave(false); }
  };

  const dialogs = <>
    {pending && saveAsName === null && <Dialog title={t("library.dirtyTitle")} description={t("library.dirtyBody")} closeLabel={t("library.cancel")} onClose={cancelPending}>
      {error && <p role="alert" className="mb-3 break-words text-xs text-danger">{error}</p>}
      <div className="flex flex-wrap justify-end gap-2">
        <Button disabled={busy} onClick={cancelPending}>{t("library.cancel")}</Button>
        <Button disabled={busy} onClick={proceed}>{t("library.discard")}</Button>
        <Button disabled={busy} onClick={() => { setContinueAfterSave(true); setSaveAsName(script.name); }}>{t("library.saveAs")}</Button>
        <Button variant="primary" disabled={busy} onClick={() => void saveAndProceed()}>{t("library.save")}</Button>
      </div>
    </Dialog>}
    {saveAsName !== null && <Dialog title={t("library.saveAs")} description={t("library.saveAsBody")} closeLabel={t("library.cancel")} onClose={cancelSaveAs}>
      <label className="grid gap-2 text-sm">{t("test.name")}<input aria-label={t("library.copyName")} maxLength={160} value={saveAsName} onChange={(event) => setSaveAsName(event.target.value)} className="min-h-11 min-w-0 rounded-lg border border-line bg-panel2 px-3" /></label>
      {error && <p role="alert" className="mt-3 break-words text-xs text-danger">{error}</p>}
      <div className="mt-4 flex flex-wrap justify-end gap-2"><Button disabled={busy} onClick={cancelSaveAs}>{t("library.cancel")}</Button><Button variant="primary" disabled={busy || !saveAsName.trim()} onClick={() => void saveAsAndProceed()}>{t("library.saveAs")}</Button></div>
    </Dialog>}
  </>;
  return { script, activeId, documentEpoch, dirty, workflows, draftState, error, draftError, validationError, busy, change, replace, guard, refresh, save, open, remove, dialogs,
    newWorkflow: () => guard(() => replace(defaultScript(defaultName))),
    saveAs: () => setSaveAsName(script.name),
    importWorkflow: (next: TestScript, after?: () => void) => guard(() => { replace(next, null, ""); after?.(); }),
  };
}

export function WorkflowLibraryControls({ editor, onNew, disabled = false }: { editor: ReturnType<typeof useWorkflowEditor>; onNew?: () => void; disabled?: boolean }) {
  const { t } = useI18n();
  const [selectedId, setSelectedId] = useState(editor.activeId ?? "");
  useEffect(() => { setSelectedId(editor.activeId ?? ""); }, [editor.activeId]);
  const blocked = editor.busy || disabled;
  return <section aria-label={t("library.title")} className="shrink-0 border-b border-line/60 bg-panel px-3 py-3" data-testid="workflow-library">
    <div className="flex min-w-0 flex-wrap items-center gap-2">
      <label className="flex min-w-0 flex-1 flex-wrap items-center gap-2 text-xs font-semibold"><FolderOpen size={14} />{t("library.title")}
        <select aria-label={t("library.select")} value={selectedId} disabled={blocked} onChange={(event) => setSelectedId(event.target.value)} className="min-h-9 min-w-0 max-w-full flex-1 rounded-lg border border-line bg-panel2 px-2 text-xs">
          <option value="">{t("library.unsaved")}</option>{editor.workflows.map((record) => <option key={record.workflow_id} value={record.workflow_id}>{record.name}</option>)}
        </select>
      </label>
      <Button className="min-h-9 py-1 text-xs" disabled={blocked || !selectedId} onClick={() => { const record = editor.workflows.find((row) => row.workflow_id === selectedId); if (record) editor.open(record); }}>{t("library.open")}</Button>
      <Badge tone={editor.dirty ? "warn" : "neutral"}>{t(editor.dirty ? "library.dirty" : "library.unchanged")}</Badge>
      <Button className="min-h-9 py-1 text-xs" disabled={blocked} onClick={onNew ?? editor.newWorkflow}><Plus size={13} />{t("test.new")}</Button>
      <Button className="min-h-9 py-1 text-xs" disabled={blocked} onClick={() => void editor.save()}><Save size={13} />{t("library.save")}</Button>
      <Button className="min-h-9 py-1 text-xs" disabled={blocked} onClick={editor.saveAs}>{t("library.saveAs")}</Button>
      {editor.activeId && <Button className="min-h-9 py-1 text-xs text-danger" disabled={blocked} aria-label={t("library.delete")} onClick={() => { const record = editor.workflows.find((row) => row.workflow_id === editor.activeId); if (record) void editor.remove(record); }}><Trash2 size={13} /></Button>}
      <Button className="min-h-9 py-1 text-xs" disabled={blocked} onClick={() => void editor.refresh()}>{t("task.refresh")}</Button>
    </div>
    <p className="mt-1 text-[11px] text-ink-dim">{t("library.localHint")}{editor.activeId && <span className="ml-2 break-all font-mono">{editor.activeId}</span>}</p>
    {(editor.error || editor.draftError) && <p role="alert" className="mt-2 break-words text-xs text-danger">{editor.error ?? editor.draftError}</p>}
    {editor.validationError && <p role="status" data-testid="workflow-validation" className="mt-2 break-words text-xs text-warn">{editor.validationError}</p>}
    {editor.dialogs}
  </section>;
}
