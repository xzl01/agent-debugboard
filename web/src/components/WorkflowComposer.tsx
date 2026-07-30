import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
} from "react";
import {
  CheckCircle2,
  CircleDot,
  Download,
  FileUp,
  Package,
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
  runDisabled = false,
}: WorkflowComposerProps) {
  const { t } = useI18n();
  const fileRef = useRef<HTMLInputElement>(null);
  const [selectedId, setSelectedId] = useState<string | null>(script.steps[0]?.id ?? null);
  const [groupSelection, setGroupSelection] = useState<Set<string>>(new Set());
  const [unitName, setUnitName] = useState("");
  const [importError, setImportError] = useState<string | null>(null);
  const [mutationError, setMutationError] = useState<string | null>(null);
  const [showGroupHint, setShowGroupHint] = useState(false);
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
      <div className="flex shrink-0 flex-wrap items-center gap-2 border-b border-line/60 bg-panel px-4 py-3">
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
        <Button variant="ghost" onClick={handleUndo} disabled={!canUndo} title="Ctrl+Z">
          <Undo2 size={15} />
        </Button>
        <Button variant="ghost" onClick={handleRedo} disabled={!canRedo} title="Ctrl+Shift+Z">
          <Redo2 size={15} />
        </Button>
        <Button variant="ghost" onClick={() => fileRef.current?.click()}>
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
        <Button variant="ghost" onClick={handleExport} disabled={script.steps.length === 0}>
          <Download size={15} />
          {t("test.export")}
        </Button>
        <Button
          variant="primary"
          onClick={onRun}
          disabled={executionCount === 0 || runDisabled}
        >
          <Play size={15} />
          {t("test.run")}
        </Button>
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
      <div className="grid min-h-[680px] flex-1 grid-cols-1 xl:min-h-0 xl:overflow-hidden xl:grid-cols-[210px_minmax(360px,1fr)_290px]">
        <Palette
          executionCount={executionCount}
          customUnits={customUnits}
          dragDispatch={dragDispatch}
          onAddStep={(type) => addStep(type)}
          onAddLoop={() => addLoop()}
          onAddCondition={() => addCondition()}
          onAddCustomUnit={(unit) => addCustomUnit(unit)}
          onDeleteCustomUnit={removeCustomUnit}
          onShowGroupHint={() => setShowGroupHint(true)}
        />

        {/* Canvas */}
        <main
          className="min-w-0 bg-bg/45 px-3 py-5 sm:px-6 xl:min-h-0 xl:overflow-x-hidden xl:overflow-y-auto xl:overscroll-contain"
          aria-label={t("test.workflow.canvas")}
          onDragEnter={handleCanvasDragEnter}
          onDragLeave={handleCanvasDragLeave}
        >
          <div className="mx-auto flex max-w-2xl items-center gap-3 rounded-2xl border border-ok/30 bg-ok/[0.06] px-4 py-3">
            <span className="grid h-9 w-9 place-items-center rounded-xl bg-ok/15 text-ok">
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

          <div className="mx-auto flex max-w-2xl items-center gap-3 rounded-2xl border border-line/70 bg-panel px-4 py-3">
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
  );
}
