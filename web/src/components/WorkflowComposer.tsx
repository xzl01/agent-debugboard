import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
} from "react";
import {
  Check,
  CheckCircle2,
  CircleAlert,
  CircleDot,
  Download,
  FilePlus2,
  FileUp,
  Package,
  PanelLeft,
  PanelRight,
  Play,
  Plus,
  Redo2,
  Repeat2,
  Undo2,
} from "lucide-react";
import { Badge, Button } from "./ui";
import { Palette } from "./workflow/Palette";
import { WorkflowNode, WorkflowDropZone } from "./workflow/CanvasNode";
import { InspectorPanel } from "./workflow/Inspector";
import type {
  CustomUnitTemplate,
  StepType,
  TestCondition,
  TestLoop,
  TestScriptItem,
  TestStep,
  WorkflowComposerProps,
} from "./workflow/types";
import { inputClass } from "./workflow/types";
import {
  cloneItem,
  cloneStep,
  createConditionCheck,
  flattenGroupItems,
  unitFromTemplate,
} from "./workflow/utils";
import { useCustomUnits } from "./workflow/useCustomUnits";
import { useWorkflowDragDrop } from "./workflow/useWorkflowDragDrop";
import { useWorkflowHistory } from "./workflow/useWorkflowHistory";
import {
  MAX_EXECUTION_STEPS,
  countScriptCommands,
  defaultStepParams,
  generateConditionId,
  generateLoopId,
  generateStepId,
  isTestCondition,
  isTestLoop,
  isTestUnit,
  parseTestScript,
  removeNestedItemFromScript,
  serializeTestScript,
  tryBuildExecutionPlan,
} from "@/lib/testScript";
import { downloadBlob } from "@/lib/utils";
import { useI18n } from "@/lib/i18n";

// ─── Main component ────────────────────────────────────────────────────────────

export type { WorkflowComposerProps };

export function WorkflowComposer({
  script,
  onChange,
  onRun,
  onNew,
  draftState,
  runDisabled = false,
  runDisabledReason,
}: WorkflowComposerProps) {
  const { t } = useI18n();
  const fileRef = useRef<HTMLInputElement>(null);
  const [selectedId, setSelectedId] = useState<string | null>(script.steps[0]?.id ?? null);
  const [groupSelection, setGroupSelection] = useState<Set<string>>(new Set());
  const [unitName, setUnitName] = useState("");
  const [importError, setImportError] = useState<string | null>(null);
  const [mutationError, setMutationError] = useState<string | null>(null);
  const [showGroupHint, setShowGroupHint] = useState(false);
  const [showLibrary, setShowLibrary] = useState(true);
  const [showInspector, setShowInspector] = useState(true);
  const [mobileView, setMobileView] = useState<"canvas" | "library" | "inspector">("canvas");
  const { customUnits, addCustomUnitTemplate, removeCustomUnit } = useCustomUnits();
  const { push: pushHistory, undo, redo, canUndo, canRedo } = useWorkflowHistory();

  // Keep a ref mirror of script for stable callbacks
  const scriptRef = useRef(script);
  scriptRef.current = script;
  const nameHistoryPushedRef = useRef(false);

  // Sync selectedId when script changes externally
  useEffect(() => {
    if (selectedId && !script.steps.some((s) => s.id === selectedId)) {
      setSelectedId(script.steps[0]?.id ?? null);
    }
  }, [script.steps, selectedId]);

  // ─── Derived state ─────────────────────────────────────────────────────────

  const execution = useMemo(() => tryBuildExecutionPlan(script), [script]);
  const executionCount = execution.plan.length;
  const commandCount = countScriptCommands(script);
  const selectedIndex = script.steps.findIndex((item) => item.id === selectedId);
  const selectedItem = selectedIndex >= 0 ? script.steps[selectedIndex] : undefined;
  const selectedIndexes = useMemo(
    () => script.steps
      .map((item, index) => groupSelection.has(item.id) ? index : -1)
      .filter((index) => index >= 0),
    [groupSelection, script.steps],
  );
  const groupSelectionIsValid = selectedIndexes.length > 0
    && selectedIndexes.every((index, offset) => index === selectedIndexes[0] + offset)
    && selectedIndexes.every((index) => {
      const item = script.steps[index];
      return (!isTestLoop(item) && !isTestCondition(item)) || isTestUnit(item);
    });
  const workspaceColumns = showLibrary && showInspector
    ? "xl:grid-cols-[236px_minmax(480px,1fr)_310px] 2xl:grid-cols-[282px_minmax(598px,1fr)_344px]"
    : showLibrary
      ? "xl:grid-cols-[236px_minmax(480px,1fr)] 2xl:grid-cols-[282px_minmax(598px,1fr)]"
      : showInspector
        ? "xl:grid-cols-[minmax(480px,1fr)_310px] 2xl:grid-cols-[minmax(598px,1fr)_344px]"
        : "xl:grid-cols-1";

  // ─── Core mutation helper (pushes undo history) ─────────────────────────────

  const commitSteps = useCallback((steps: TestScriptItem[]): boolean => {
    const candidate = { ...scriptRef.current, steps };
    const attempt = tryBuildExecutionPlan(candidate);
    if (attempt.error?.includes(`exceeds ${MAX_EXECUTION_STEPS}`)) {
      setMutationError(attempt.error);
      return false;
    }
    setMutationError(null);
    pushHistory(scriptRef.current);
    onChange(candidate);
    return true;
  }, [onChange, pushHistory]);

  // ─── Step operations ─────────────────────────────────────────────────────────

  const addStep = useCallback((type: StepType, index = script.steps.length) => {
    if (executionCount >= MAX_EXECUTION_STEPS) return;
    const step: TestStep = { id: generateStepId(), type, params: defaultStepParams(type) };
    const steps = [...script.steps];
    steps.splice(index, 0, step);
    if (!commitSteps(steps)) return;
    setSelectedId(step.id);
  }, [commitSteps, executionCount, script.steps]);

  const addLoop = useCallback((index = script.steps.length) => {
    const child: TestStep = { id: generateStepId(), type: "delay", params: defaultStepParams("delay") };
    const loop: TestLoop = { id: generateLoopId(), type: "loop", params: { count: 2, steps: [child] } };
    const steps = [...script.steps];
    steps.splice(index, 0, loop);
    if (!commitSteps(steps)) return;
    setSelectedId(loop.id);
  }, [commitSteps, script.steps]);

  const addCondition = useCallback((index = script.steps.length) => {
    const condition: TestCondition = {
      id: generateConditionId(),
      type: "condition",
      params: {
        check: createConditionCheck("serial_expect"),
        then_steps: [{ id: generateStepId(), type: "delay", params: defaultStepParams("delay") }],
        else_steps: [],
      },
    };
    const steps = [...script.steps];
    steps.splice(index, 0, condition);
    if (!commitSteps(steps)) return;
    setSelectedId(condition.id);
  }, [commitSteps, script.steps]);

  const addCustomUnit = useCallback((template: CustomUnitTemplate, index = script.steps.length) => {
    const unit = unitFromTemplate(template);
    const steps = [...script.steps];
    steps.splice(index, 0, unit);
    if (!commitSteps(steps)) return;
    setSelectedId(unit.id);
  }, [commitSteps, script.steps]);

  const moveItem = useCallback((index: number, offset: number) => {
    const to = index + offset;
    if (index < 0 || index >= script.steps.length || to < 0 || to >= script.steps.length) return;
    const steps = [...script.steps];
    const [item] = steps.splice(index, 1);
    steps.splice(to, 0, item);
    commitSteps(steps);
  }, [commitSteps, script.steps]);

  const updateItem = useCallback((index: number, item: TestScriptItem) => {
    const steps = [...script.steps];
    steps[index] = item;
    commitSteps(steps);
  }, [commitSteps, script.steps]);

  const deleteItem = useCallback((index: number) => {
    const item = script.steps[index];
    if (!item) return;
    const steps = script.steps.filter((_, current) => current !== index);
    commitSteps(steps);
    setGroupSelection((current) => {
      const next = new Set(current);
      next.delete(item.id);
      return next;
    });
    if (item.id === selectedId) setSelectedId(steps[Math.min(index, steps.length - 1)]?.id ?? null);
  }, [commitSteps, script.steps, selectedId]);

  const duplicateItem = useCallback((index: number) => {
    const duplicate = cloneItem(script.steps[index]);
    const steps = [...script.steps];
    steps.splice(index + 1, 0, duplicate);
    if (!commitSteps(steps)) return;
    setSelectedId(duplicate.id);
  }, [commitSteps, script.steps]);

  const toggleGroupSelection = useCallback((index: number) => {
    const itemId = script.steps[index]?.id;
    if (!itemId) return;
    setGroupSelection((current) => {
      const next = new Set(current);
      if (next.has(itemId)) next.delete(itemId);
      else next.add(itemId);
      return next;
    });
  }, [script.steps]);

  const createGroup = useCallback((kind: "unit" | "loop") => {
    if (!groupSelectionIsValid) return;
    const firstIndex = selectedIndexes[0];
    const selectedItems = selectedIndexes.map((index) => script.steps[index]);
    const steps = flattenGroupItems(selectedItems);
    const group: TestLoop = {
      id: generateLoopId(),
      type: "loop",
      params: {
        count: kind === "unit" ? 1 : 2,
        steps,
        ...(kind === "unit" ? { unit: { name: unitName.trim() || t("test.unit.defaultName") } } : {}),
      },
    };
    const next = [...script.steps];
    next.splice(firstIndex, selectedItems.length, group);
    if (!commitSteps(next)) return;
    if (kind === "unit") {
      const templateName = unitName.trim() || t("test.unit.defaultName");
      addCustomUnitTemplate({
        id: generateLoopId(),
        name: templateName,
        steps: steps.map(cloneStep),
      });
    }
    setGroupSelection(new Set());
    setUnitName("");
    setSelectedId(group.id);
    setShowGroupHint(false);
  }, [addCustomUnitTemplate, commitSteps, groupSelectionIsValid, script.steps, selectedIndexes, t, unitName]);

  const ungroupSelected = useCallback(() => {
    if (!selectedItem || !isTestLoop(selectedItem) || selectedIndex < 0) return;
    const steps = [...script.steps];
    steps.splice(selectedIndex, 1, ...selectedItem.params.steps);
    if (!commitSteps(steps)) return;
    setSelectedId(selectedItem.params.steps[0]?.id ?? null);
  }, [commitSteps, script.steps, selectedIndex, selectedItem]);

  const handleSelect = useCallback((index: number) => {
    setSelectedId(scriptRef.current.steps[index]?.id ?? null);
    setMobileView("inspector");
  }, []);

  const selectItem = useCallback((id: string) => setSelectedId(id), []);
  const removeNestedItem = useCallback((
    containerId: string,
    branch: "body" | "then" | "else",
    nestedItemId: string,
  ) => {
    const steps = removeNestedItemFromScript(
      scriptRef.current.steps,
      containerId,
      branch,
      nestedItemId,
    );
    if (!steps) return;
    if (!commitSteps(steps)) return;
    setSelectedId(containerId);
  }, [commitSteps]);
  const {
    drag,
    dragDispatch,
    handleDrop,
    handleNestedItemDrop,
    handleNodeDragStart,
    handleCanvasDragEnter,
    handleCanvasDragLeave,
    handleDragEnd,
    handleNestedDragEnter,
  } = useWorkflowDragDrop({
    script,
    customUnits,
    commitSteps,
    addStep,
    addCustomUnit,
    selectItem,
  });

  // ─── Import / Export ─────────────────────────────────────────────────────────

  const handleImport = useCallback((event: React.ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = () => {
      try {
        const imported = parseTestScript(String(reader.result));
        setImportError(null);
        setMutationError(null);
        setGroupSelection(new Set());
        setSelectedId(imported.steps[0]?.id ?? null);
        pushHistory(script);
        onChange(imported);
      } catch (error) {
        setImportError(error instanceof Error ? error.message : String(error));
      }
    };
    reader.onerror = () => setImportError(t("test.workflow.importReadError"));
    reader.readAsText(file);
    event.target.value = "";
  }, [onChange, t, script, pushHistory]);

  const handleExport = useCallback(() => {
    downloadBlob(
      `${script.name.replace(/\s+/g, "-") || "workflow"}.ndjson`,
      serializeTestScript(script),
      "application/x-ndjson",
    );
  }, [script]);

  // ─── Undo / Redo ─────────────────────────────────────────────────────────────

  const handleUndo = useCallback(() => {
    const restored = undo(script);
    if (restored) onChange(restored);
  }, [undo, script, onChange]);

  const handleRedo = useCallback(() => {
    const restored = redo(script);
    if (restored) onChange(restored);
  }, [redo, script, onChange]);

  // ─── Keyboard shortcuts ──────────────────────────────────────────────────────

  useEffect(() => {
    const handler = (event: KeyboardEvent) => {
      const target = event.target as HTMLElement;
      if (target.tagName === "INPUT" || target.tagName === "TEXTAREA" || target.tagName === "SELECT" || target.isContentEditable) {
        return;
      }
      const mod = event.metaKey || event.ctrlKey;
      if (mod && event.key === "z" && !event.shiftKey) {
        event.preventDefault();
        handleUndo();
      } else if (mod && (event.key === "Z" || (event.key === "z" && event.shiftKey))) {
        event.preventDefault();
        handleRedo();
      } else if (mod && event.key === "d") {
        event.preventDefault();
        if (selectedIndex >= 0) duplicateItem(selectedIndex);
      } else if ((event.key === "Delete" || event.key === "Backspace") && selectedIndex >= 0) {
        event.preventDefault();
        deleteItem(selectedIndex);
      } else if (event.altKey && event.key === "ArrowUp") {
        event.preventDefault();
        if (selectedIndex >= 0) moveItem(selectedIndex, -1);
      } else if (event.altKey && event.key === "ArrowDown") {
        event.preventDefault();
        if (selectedIndex >= 0) moveItem(selectedIndex, 1);
      } else if (event.key === "Escape") {
        setGroupSelection(new Set());
        setShowGroupHint(false);
      }
    };
    window.addEventListener("keydown", handler);
    return () => window.removeEventListener("keydown", handler);
  }, [handleUndo, handleRedo, duplicateItem, deleteItem, moveItem, selectedIndex]);

  // ─── Render ──────────────────────────────────────────────────────────────────

  return (
    <div className="flex min-h-0 flex-col xl:h-full xl:overflow-hidden">
      {/* Toolbar */}
      <div className="shrink-0 space-y-2 border-b border-line/60 bg-panel px-4 py-3">
        <div className="flex flex-wrap items-center gap-2">
          <input
            className={`${inputClass} min-w-[180px] flex-1 font-semibold sm:max-w-md`}
            value={script.name}
            onFocus={() => {
              nameHistoryPushedRef.current = false;
            }}
            onChange={(event) => {
              if (!nameHistoryPushedRef.current) {
                pushHistory(scriptRef.current);
                nameHistoryPushedRef.current = true;
              }
              onChange({ ...script, name: event.target.value });
            }}
            placeholder={t("test.name")}
            aria-label={t("test.name")}
          />
          <Badge tone="neutral">
            {t("test.stepCountExpanded", { commands: commandCount, executions: executionCount })}
          </Badge>
          {draftState && (
            <Badge tone={draftState === "error" ? "danger" : draftState === "saved" ? "ok" : "neutral"}>
              {draftState === "error" ? <CircleAlert size={12} /> : <Check size={12} />}
              {t(`test.draft.${draftState}`)}
            </Badge>
          )}
          <Button
            variant="primary"
            className="ml-auto"
            onClick={onRun}
            disabled={executionCount === 0 || runDisabled}
            aria-describedby={runDisabledReason ? "workflow-run-disabled-reason" : undefined}
          >
            <Play size={15} />
            {t("test.run")}
          </Button>
        </div>

        {runDisabledReason && (
          <p
            id="workflow-run-disabled-reason"
            role="status"
            className="flex items-center gap-1.5 text-xs text-warn"
          >
            <CircleAlert size={13} />
            {runDisabledReason}
          </p>
        )}

        <div className="flex flex-wrap items-center justify-between gap-2">
          <div
            role="group"
            aria-label={t("test.workflow.layoutControls")}
            className="hidden rounded-xl border border-line/70 bg-panel2/55 p-1 xl:inline-flex"
          >
            <Button
              variant="ghost"
              className={showLibrary ? "min-h-8 bg-brand/10 py-1 text-xs text-brand ring-1 ring-inset ring-brand/15" : "min-h-8 py-1 text-xs"}
              aria-pressed={showLibrary}
              onClick={() => setShowLibrary((visible) => !visible)}
            >
              <PanelLeft size={14} />
              {t("test.workflow.library")}
            </Button>
            <Button
              variant="ghost"
              className={showInspector ? "min-h-8 bg-brand/10 py-1 text-xs text-brand ring-1 ring-inset ring-brand/15" : "min-h-8 py-1 text-xs"}
              aria-pressed={showInspector}
              onClick={() => setShowInspector((visible) => !visible)}
            >
              <PanelRight size={14} />
              {t("test.workflow.inspector")}
            </Button>
          </div>

          <div
            role="group"
            aria-label={t("test.workflow.mobilePanels")}
            className="grid w-full grid-cols-3 rounded-xl border border-line/70 bg-panel2/55 p-1 xl:hidden"
          >
            {([
              ["library", PanelLeft, t("test.workflow.library")],
              ["canvas", CircleDot, t("test.workflow.canvasShort")],
              ["inspector", PanelRight, t("test.workflow.inspector")],
            ] as const).map(([view, Icon, label]) => (
              <Button
                key={view}
                variant="ghost"
                className={mobileView === view ? "min-h-11 bg-brand/10 px-2 py-1.5 text-xs text-brand ring-1 ring-inset ring-brand/15" : "min-h-11 px-2 py-1.5 text-xs"}
                aria-pressed={mobileView === view}
                onClick={() => setMobileView(view)}
              >
                <Icon size={14} />
                <span className="truncate">{label}</span>
              </Button>
            ))}
          </div>

          <div className="flex flex-wrap items-center gap-1">
            <div className="inline-flex rounded-xl border border-line/70 bg-panel2/55 p-1">
              <Button
                variant="ghost"
                className="min-h-8 min-w-8 px-2 py-1"
                onClick={handleUndo}
                disabled={!canUndo}
                title="Ctrl+Z"
                aria-label={t("test.undo")}
              >
                <Undo2 size={15} />
              </Button>
              <Button
                variant="ghost"
                className="min-h-8 min-w-8 px-2 py-1"
                onClick={handleRedo}
                disabled={!canRedo}
                title="Ctrl+Shift+Z"
                aria-label={t("test.redo")}
              >
                <Redo2 size={15} />
              </Button>
            </div>
            {onNew && (
              <Button variant="ghost" className="min-h-8 py-1 text-xs" onClick={onNew}>
                <FilePlus2 size={15} />
                {t("test.new")}
              </Button>
            )}
            <Button variant="ghost" className="min-h-8 py-1 text-xs" onClick={() => fileRef.current?.click()}>
              <FileUp size={15} />
              {t("test.import")}
            </Button>
            <input
              ref={fileRef}
              type="file"
              accept=".ndjson,.jsonl,.json"
              className="hidden"
              onChange={handleImport}
            />
            <Button
              variant="ghost"
              className="min-h-8 py-1 text-xs"
              onClick={handleExport}
              disabled={script.steps.length === 0}
            >
              <Download size={15} />
              {t("test.export")}
            </Button>
          </div>
        </div>
      </div>

      {script.steps.length === 0 && (
        <div role="status" className="mx-4 mt-3 shrink-0 rounded-xl border border-line/60 bg-panel2/40 px-3 py-2 text-xs text-ink-dim">
          {t("test.noSteps")}
        </div>
      )}

      {(importError || mutationError || execution.error) && (
        <div role="alert" className="mx-4 mt-3 shrink-0 rounded-xl border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
          {importError || mutationError || execution.error}
        </div>
      )}

      {/* Group selection bar */}
      {groupSelection.size > 0 && (
        <div className="mx-4 mt-3 flex shrink-0 flex-wrap items-center gap-2 rounded-xl border border-brand/30 bg-brand/[0.06] p-2.5">
          <Package size={15} className="text-brand" />
          <span className="text-xs text-ink-dim">
            {t("test.workflow.groupSelected", { n: groupSelection.size })}
          </span>
          {!groupSelectionIsValid && (
            <span className="text-xs text-warn">{t("test.loop.nonContiguous")}</span>
          )}
          <input
            className={`${inputClass} min-w-[180px] flex-1 sm:max-w-xs`}
            value={unitName}
            onChange={(event) => setUnitName(event.target.value)}
            placeholder={t("test.unit.namePlaceholder")}
            aria-label={t("test.unit.name")}
          />
          <Button variant="default" onClick={() => createGroup("unit")} disabled={!groupSelectionIsValid}>
            <Package size={14} />
            {t("test.unit.create")}
          </Button>
          <Button variant="default" onClick={() => createGroup("loop")} disabled={!groupSelectionIsValid}>
            <Repeat2 size={14} />
            {t("test.loop.create")}
          </Button>
          <Button variant="ghost" onClick={() => setGroupSelection(new Set())}>
            {t("test.loop.cancelSelection")}
          </Button>
        </div>
      )}

      {showGroupHint && groupSelection.size === 0 && (
        <div role="status" className="mx-4 mt-3 flex shrink-0 items-center gap-2 rounded-xl border border-brand/30 bg-brand/[0.06] px-3 py-2 text-xs text-ink-dim">
          <Package size={15} className="text-brand" />
          <span className="min-w-0 flex-1">{t("test.workflow.packageHint")}</span>
          <Button variant="ghost" className="min-h-8 py-1 text-xs" onClick={() => setShowGroupHint(false)}>
            {t("test.loop.cancelSelection")}
          </Button>
        </div>
      )}

      {/* Three-column layout */}
      <div className={`grid min-h-[680px] flex-1 grid-cols-1 gap-3 bg-bg/45 p-3 xl:min-h-0 xl:overflow-hidden ${workspaceColumns}`}>
        <div
          data-testid="workflow-library-panel"
          className={`${mobileView === "library" ? "block" : "hidden"} ${showLibrary ? "xl:contents" : "xl:hidden"}`}
        >
          <Palette
            executionCount={executionCount}
            customUnits={customUnits}
            dragDispatch={dragDispatch}
            onAddStep={(type) => {
              addStep(type);
              setMobileView("inspector");
            }}
            onAddLoop={() => {
              addLoop();
              setMobileView("inspector");
            }}
            onAddCondition={() => {
              addCondition();
              setMobileView("inspector");
            }}
            onAddCustomUnit={(unit) => {
              addCustomUnit(unit);
              setMobileView("inspector");
            }}
            onDeleteCustomUnit={removeCustomUnit}
            onShowGroupHint={() => setShowGroupHint(true)}
          />
        </div>

        {/* Canvas */}
        <main
          data-testid="workflow-canvas-panel"
          className={`${mobileView === "canvas" ? "block" : "hidden"} min-w-0 rounded-2xl border border-line/70 bg-panel px-3 py-4 sm:px-5 xl:block xl:min-h-0 xl:overflow-x-hidden xl:overflow-y-auto xl:overscroll-contain`}
          aria-label={t("test.workflow.canvas")}
          onDragEnter={handleCanvasDragEnter}
          onDragLeave={handleCanvasDragLeave}
        >
          <header className="mx-auto mb-3 flex w-full max-w-[598px] items-end justify-between gap-3">
            <div className="min-w-0">
              <h2 className="text-sm font-semibold text-ink">{t("test.workflow.canvas")}</h2>
              <p className="mt-0.5 truncate text-[10px] text-ink-dim">
                {t("test.stepCountExpanded", { commands: commandCount, executions: executionCount })}
              </p>
            </div>
          </header>

          <div className="mx-auto flex max-w-[506px] items-center gap-3 rounded-[14px] border border-line/70 bg-panel px-3 py-2.5">
            <span className="grid h-9 w-9 place-items-center rounded-xl bg-panel2 text-ok">
              <CircleDot size={17} />
            </span>
            <span className="min-w-0 flex-1">
              <span className="block text-xs font-semibold text-ink">{t("test.workflow.start")}</span>
              <span className="block text-[10px] text-ink-dim">{t("test.workflow.startHint")}</span>
            </span>
            <Badge tone="ok">Trigger</Badge>
          </div>

          <WorkflowDropZone
            active={drag.overIndex === 0}
            dragging={drag.active}
            label={t("test.workflow.dropHere")}
            onDragEnter={() => dragDispatch({ type: "over", index: 0 })}
            onDrop={(event) => handleDrop(event, 0)}
          />

          {script.steps.map((item, index) => (
            <div key={item.id}>
              <WorkflowNode
                item={item}
                index={index}
                total={script.steps.length}
                selected={selectedId === item.id}
                groupSelected={groupSelection.has(item.id)}
                dragging={drag.itemId === item.id}
                drag={drag}
                onSelect={handleSelect}
                onToggleGroup={toggleGroupSelection}
                onMove={moveItem}
                onDuplicate={duplicateItem}
                onDelete={deleteItem}
                onDragStart={handleNodeDragStart}
                onDragEnd={handleDragEnd}
                onNestedDragEnter={handleNestedDragEnter}
                onDropNestedItem={handleNestedItemDrop}
                onRemoveNestedItem={removeNestedItem}
              />
              <WorkflowDropZone
                active={drag.overIndex === index + 1}
                dragging={drag.active}
                label={t("test.workflow.dropHere")}
                onDragEnter={() => dragDispatch({ type: "over", index: index + 1 })}
                onDrop={(event) => handleDrop(event, index + 1)}
              />
            </div>
          ))}

          {script.steps.length === 0 && (
            <button
              type="button"
              onClick={() => addStep("power_on")}
              onDragOver={(event) => event.preventDefault()}
              onDrop={(event) => handleDrop(event, 0)}
              className="mx-auto mb-8 flex min-h-44 w-full max-w-xl flex-col items-center justify-center gap-3 rounded-2xl border border-dashed border-line bg-panel/45 px-6 text-center transition-colors hover:border-brand/50 hover:bg-brand/[0.03] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40"
            >
              <span className="grid h-11 w-11 place-items-center rounded-2xl bg-brand/10 text-brand">
                <Plus size={19} />
              </span>
              <span>
                <span className="block text-sm font-semibold text-ink">{t("test.workflow.empty")}</span>
                <span className="mt-1 block text-xs text-ink-dim">{t("test.workflow.emptyHint")}</span>
              </span>
            </button>
          )}

          <div className="mx-auto flex max-w-[506px] items-center gap-3 rounded-[14px] border border-line/70 bg-panel px-3 py-2.5">
            <span className="grid h-9 w-9 place-items-center rounded-xl bg-panel2 text-ink-dim">
              <CheckCircle2 size={17} />
            </span>
            <span className="min-w-0 flex-1">
              <span className="block text-xs font-semibold text-ink">{t("test.workflow.finish")}</span>
              <span className="block text-[10px] text-ink-dim">{t("test.workflow.finishHint")}</span>
            </span>
            <Badge tone="neutral">Report</Badge>
          </div>
        </main>

        <div
          data-testid="workflow-inspector-panel"
          className={`${mobileView === "inspector" ? "block" : "hidden"} ${showInspector ? "xl:contents" : "xl:hidden"}`}
        >
          <InspectorPanel
            selectedItem={selectedItem}
            selectedIndex={selectedIndex}
            customUnits={customUnits}
            onUpdateItem={updateItem}
            onDuplicate={duplicateItem}
            onDelete={deleteItem}
            onUngroup={ungroupSelected}
          />
        </div>
      </div>
    </div>
  );
}
