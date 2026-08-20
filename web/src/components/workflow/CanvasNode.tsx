import { memo, type DragEvent } from "react";
import {
  CheckCircle2,
  ChevronDown,
  ChevronUp,
  Copy,
  GitFork,
  GripVertical,
  HelpCircle,
  Package,
  Repeat2,
  Trash2,
  X,
} from "lucide-react";
import { Badge } from "../ui";
import { TestStepInspector, testStepSummary } from "../TestEditor";
import type {
  DragState,
  NestedDropBranch,
  TestCondition,
  TestLoop,
  TestNestedItem,
  TestScriptItem,
  TestStep,
} from "./types";
import { MAX_NESTED_PREVIEW_STEPS } from "./types";
import { STEP_ICONS, setDragPayload } from "./utils";
import { isTestCondition, isTestLoop, isTestUnit } from "@/lib/testScript";
import { useI18n } from "@/lib/i18n";

// ─── Nested step preview rows ──────────────────────────────────────────────────

function NestedStepRows({
  steps,
  onRemove,
}: {
  steps: TestNestedItem[];
  onRemove: (itemId: string) => void;
}) {
  const { t } = useI18n();
  const visibleSteps = steps.slice(0, MAX_NESTED_PREVIEW_STEPS);
  const hiddenCount = steps.length - visibleSteps.length;

  if (steps.length === 0) {
    return (
      <div className="rounded-lg border border-dashed border-line/70 px-3 py-2 text-[10px] text-ink-dim">
        {t("test.workflow.emptyBranch")}
      </div>
    );
  }

  return (
    <div role="list" className="space-y-1.5">
      {visibleSteps.map((step, index) => {
        const unit = isTestLoop(step) ? step : null;
        const Icon = unit ? Package : STEP_ICONS[(step as TestStep).type];
        const itemName = unit
          ? unit.params.unit?.name ?? t("test.unit.defaultName")
          : t(`test.step.${(step as TestStep).type}`);
        return (
          <div
            key={step.id}
            role="listitem"
            data-workflow-nested-item-id={step.id}
            className="flex min-h-9 items-center gap-2 rounded-lg border border-line/55 bg-panel/80 px-2.5 py-1.5"
          >
            <span className="w-4 shrink-0 text-right font-mono text-[9px] tabular-nums text-ink-dim">
              {index + 1}
            </span>
            <Icon size={13} className={`shrink-0 ${unit ? "text-violet-500" : "text-brand"}`} />
            <span className="min-w-0 flex-1 truncate text-[11px] font-medium text-ink">
              {itemName}
            </span>
            <span className="hidden max-w-[42%] truncate font-mono text-[9px] text-ink-dim sm:block">
              {unit
                ? t("test.unit.summary", { commands: unit.params.steps.length })
                : testStepSummary(step as TestStep)}
            </span>
            <button
              type="button"
              onClick={(event) => {
                event.stopPropagation();
                onRemove(step.id);
              }}
              className="relative grid h-8 w-8 shrink-0 place-items-center rounded-md text-ink-dim transition-colors before:absolute before:-inset-1.5 before:content-[''] hover:bg-danger/10 hover:text-danger focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-danger/40"
              aria-label={t("test.workflow.removeNestedItem", { name: itemName })}
              title={t("test.workflow.removeNestedItem", { name: itemName })}
            >
              <X size={13} />
            </button>
          </div>
        );
      })}
      {hiddenCount > 0 && (
        <div className="px-2.5 py-1 text-[10px] font-medium text-ink-dim">
          {t("test.workflow.moreSteps", { n: hiddenCount })}
        </div>
      )}
    </div>
  );
}

// ─── Nested unit drop zone ─────────────────────────────────────────────────────

function NestedItemDropZone({
  active,
  draggingItem,
  dropEffect,
}: {
  active: boolean;
  draggingItem: boolean;
  dropEffect: "copy" | "move";
}) {
  const { t } = useI18n();
  return (
    <div
      aria-hidden="true"
      className={`mt-2 flex min-h-10 items-center justify-center rounded-lg border border-dashed px-2 text-center text-[10px] font-medium transition-[border-color,background-color,color,opacity] duration-150 ${
        draggingItem
          ? active
            ? "border-brand bg-brand/10 text-brand opacity-100 ring-2 ring-brand/15"
            : "border-brand/35 bg-brand/[0.035] text-brand/75 opacity-100"
          : "border-line/45 bg-panel/35 text-ink-dim/70"
      }`}
    >
      <Package size={13} className="mr-1.5 shrink-0" />
      {t(dropEffect === "move" ? "test.workflow.moveUnitHere" : "test.workflow.dropUnitHere")}
    </div>
  );
}

// ─── Indented branch (then/else) ───────────────────────────────────────────────

function IndentedBranch({
  label,
  steps,
  tone,
  target,
  activeDropTarget,
  draggingItem,
  onDragEnter,
  onDropItem,
  onRemoveItem,
  dropEffect,
}: {
  label: string;
  steps: TestNestedItem[];
  tone: "then" | "else";
  target: string;
  activeDropTarget: string | null;
  draggingItem: boolean;
  onDragEnter: (target: string) => void;
  onDropItem: (event: DragEvent<HTMLElement>) => void;
  onRemoveItem: (itemId: string) => void;
  dropEffect: "copy" | "move";
}) {
  const lineClass = tone === "then" ? "border-ok/45" : "border-warn/45";
  const dotClass = tone === "then" ? "bg-ok" : "bg-warn";

  return (
    <section
      className={`relative border-l-2 pl-4 ${lineClass}`}
      aria-label={label}
      data-nested-drop-target={target}
      onDragEnter={(event) => {
        event.stopPropagation();
        onDragEnter(target);
      }}
      onDragOver={(event) => {
        event.preventDefault();
        event.stopPropagation();
        event.dataTransfer.dropEffect = event.dataTransfer.effectAllowed === "move" ? "move" : "copy";
      }}
      onDrop={(event) => {
        event.preventDefault();
        event.stopPropagation();
        onDropItem(event);
      }}
    >
      <span className={`absolute -left-[5px] top-3 h-2 w-2 rounded-full ring-4 ring-panel ${dotClass}`} />
      <div className="mb-1.5 flex min-h-6 items-center gap-2">
        <span className="text-[10px] font-semibold text-ink">{label}</span>
        <Badge tone="neutral">{steps.length}</Badge>
      </div>
      <NestedStepRows steps={steps} onRemove={onRemoveItem} />
      <NestedItemDropZone
        active={activeDropTarget === target}
        draggingItem={draggingItem}
        dropEffect={dropEffect}
      />
    </section>
  );
}

// ─── Flow structure preview (loop body / condition branches) ───────────────────

function FlowStructurePreview({
  item,
  drag,
  onNestedDragEnter,
  onDropItem,
  onRemoveItem,
}: {
  item: TestLoop | TestCondition;
  drag: DragState;
  onNestedDragEnter: (target: string) => void;
  onDropItem: (event: DragEvent<HTMLElement>, branch: NestedDropBranch) => void;
  onRemoveItem: (branch: NestedDropBranch, itemId: string) => void;
}) {
  const { t } = useI18n();
  const draggingItem = drag.nestable;
  const dropEffect = drag.itemId ? "move" : "copy";

  if (isTestCondition(item)) {
    const CheckIcon = STEP_ICONS[item.params.check.type];
    return (
      <div className="border-t border-line/55 bg-panel2/20 px-3 pb-3 pt-2.5">
        <div className="relative ml-3 border-l-2 border-violet-500/35 pl-4 sm:ml-8">
          <span className="absolute -left-px top-4 h-px w-4 -translate-x-full bg-violet-500/35" />
          <div className="mb-2 flex min-h-9 items-center gap-2 rounded-lg border border-violet-500/25 bg-violet-500/[0.045] px-2.5 py-1.5">
            <CheckIcon size={13} className="shrink-0 text-violet-500" />
            <span className="shrink-0 text-[10px] font-semibold text-ink">
              {t("test.condition.check")}
            </span>
            <span className="min-w-0 flex-1 truncate font-mono text-[9px] text-ink-dim">
              {t(`test.step.${item.params.check.type}`)} · {testStepSummary(item.params.check)}
            </span>
          </div>
          <div className="ml-2 space-y-3 sm:ml-4">
            <IndentedBranch
              label={t("test.condition.then")}
              steps={item.params.then_steps}
              tone="then"
              target={`${item.id}:then`}
              activeDropTarget={drag.nestedTarget}
              draggingItem={draggingItem}
              onDragEnter={onNestedDragEnter}
              onDropItem={(event) => onDropItem(event, "then")}
              onRemoveItem={(itemId) => onRemoveItem("then", itemId)}
              dropEffect={dropEffect}
            />
            <IndentedBranch
              label={t("test.condition.else")}
              steps={item.params.else_steps}
              tone="else"
              target={`${item.id}:else`}
              activeDropTarget={drag.nestedTarget}
              draggingItem={draggingItem}
              onDragEnter={onNestedDragEnter}
              onDropItem={(event) => onDropItem(event, "else")}
              onRemoveItem={(itemId) => onRemoveItem("else", itemId)}
              dropEffect={dropEffect}
            />
          </div>
        </div>
      </div>
    );
  }

  const unit = isTestUnit(item);
  return (
    <div className="border-t border-line/55 bg-panel2/20 px-3 pb-3 pt-2.5">
      <section
        className="relative ml-3 border-l-2 border-violet-500/35 pl-4 sm:ml-8"
        aria-label={unit ? t("test.unit.body") : t("test.loop.body")}
        data-nested-drop-target={!unit ? `${item.id}:body` : undefined}
        onDragEnter={!unit ? (event) => {
          event.stopPropagation();
          onNestedDragEnter(`${item.id}:body`);
        } : undefined}
        onDragOver={!unit ? (event) => {
          event.preventDefault();
          event.stopPropagation();
          event.dataTransfer.dropEffect = event.dataTransfer.effectAllowed === "move" ? "move" : "copy";
        } : undefined}
        onDrop={!unit ? (event) => {
          event.preventDefault();
          event.stopPropagation();
          onDropItem(event, "body");
        } : undefined}
      >
        <span className="absolute -left-px top-3 h-px w-4 -translate-x-full bg-violet-500/35" />
        <div className="mb-1.5 flex min-h-6 items-center gap-2">
          <span className="text-[10px] font-semibold text-ink">
            {unit ? t("test.unit.body") : t("test.loop.body")}
          </span>
          <Badge tone="brand">{unit ? item.params.steps.length : `×${item.params.count}`}</Badge>
        </div>
        <NestedStepRows
          steps={item.params.steps}
          onRemove={(itemId) => onRemoveItem("body", itemId)}
        />
        {!unit && (
          <NestedItemDropZone
            active={drag.nestedTarget === `${item.id}:body`}
            draggingItem={draggingItem}
            dropEffect={dropEffect}
          />
        )}
      </section>
    </div>
  );
}

// ─── Main workflow drop zone (between nodes) ───────────────────────────────────

export function WorkflowDropZone({
  active,
  dragging,
  label,
  onDragEnter,
  onDrop,
}: {
  active: boolean;
  dragging: boolean;
  label: string;
  onDragEnter: () => void;
  onDrop: (event: DragEvent<HTMLDivElement>) => void;
}) {
  return (
    <div
      aria-hidden="true"
      onDragEnter={onDragEnter}
      onDragOver={(event) => {
        event.preventDefault();
        event.dataTransfer.dropEffect = event.dataTransfer.effectAllowed === "copy" ? "copy" : "move";
      }}
      onDrop={onDrop}
      className={`group relative mx-auto flex w-full max-w-xl items-center justify-center transition-[height,color,opacity] duration-200 ${
        dragging ? (active ? "h-14 text-brand" : "h-10 text-brand/70") : "h-8 text-transparent"
      }`}
    >
      <span className={`absolute inset-x-8 rounded-xl transition-all duration-200 ${
        active
          ? "h-10 border-2 border-dashed border-brand/60 bg-brand/[0.08]"
          : "h-px bg-line/70 group-hover:bg-brand/50"
      }`} />
      <span className={`relative rounded-full border border-brand/30 bg-panel px-2 py-0.5 text-[10px] font-medium shadow-sm transition-opacity ${
        active ? "opacity-100" : "opacity-0 group-hover:opacity-100"
      }`}>
        {label}
      </span>
    </div>
  );
}

// ─── Item summary helper ───────────────────────────────────────────────────────

function itemSummary(item: TestScriptItem, t: ReturnType<typeof useI18n>["t"]) {
  if (isTestCondition(item)) {
    return t("test.condition.summary", {
      then: item.params.then_steps.length,
      else: item.params.else_steps.length,
    });
  }
  if (!isTestLoop(item)) return testStepSummary(item);
  if (isTestUnit(item)) {
    return t("test.unit.summary", { commands: item.params.steps.length });
  }
  return t("test.loop.summary", {
    commands: item.params.steps.length,
    executions: item.params.steps.length * item.params.count,
  });
}

// ─── Workflow node (memoized) ──────────────────────────────────────────────────

export interface WorkflowNodeProps {
  item: TestScriptItem;
  index: number;
  total: number;
  selected: boolean;
  groupSelected: boolean;
  dragging: boolean;
  drag: DragState;
  onSelect: (index: number) => void;
  onToggleGroup: (index: number) => void;
  onMove: (index: number, offset: number) => void;
  onDuplicate: (index: number) => void;
  onDelete: (index: number) => void;
  onDragStart: (event: DragEvent<HTMLButtonElement>, index: number) => void;
  onDragEnd: () => void;
  onNestedDragEnter: (target: string) => void;
  onDropNestedItem: (event: DragEvent<HTMLElement>, itemId: string, branch: NestedDropBranch) => void;
  onRemoveNestedItem: (itemId: string, branch: NestedDropBranch, nestedItemId: string) => void;
}

export const WorkflowNode = memo(function WorkflowNode({
  item,
  index,
  total,
  selected,
  groupSelected,
  dragging,
  drag,
  onSelect,
  onToggleGroup,
  onMove,
  onDuplicate,
  onDelete,
  onDragStart,
  onDragEnd,
  onNestedDragEnter,
  onDropNestedItem,
  onRemoveNestedItem,
}: WorkflowNodeProps) {
  const { t } = useI18n();
  const condition = isTestCondition(item);
  const loop = isTestLoop(item);
  const unit = isTestUnit(item);
  const groupable = (!loop && !condition) || unit;
  const Icon = condition ? GitFork : loop ? (unit ? Package : Repeat2) : STEP_ICONS[item.type];
  const title = condition
    ? t("test.condition.title")
    : loop
    ? unit
      ? item.params.unit.name
      : t("test.loop.title")
    : t(`test.step.${item.type}`);

  return (
    <article
      data-workflow-drag-preview
      data-workflow-item-id={item.id}
      data-workflow-item-kind={unit ? "unit" : condition ? "condition" : loop ? "loop" : "step"}
      className={`group relative mx-auto w-full max-w-[598px] rounded-[14px] border bg-panel shadow-sm transition-[border-color,box-shadow,background-color] duration-200 ${
        dragging
          ? "border-dashed border-brand/45 bg-brand/[0.035] opacity-25 shadow-none"
          : selected
          ? "border-brand/70 bg-panel shadow-md shadow-brand/10 ring-2 ring-brand/10"
          : "border-line/70 hover:border-brand/35 hover:shadow-md"
      }`}
    >
      <div className="flex min-h-16 items-center gap-2 p-2.5 sm:gap-3 sm:px-3">
        <button
          type="button"
          draggable
          onDragStart={(event) => onDragStart(event, index)}
          onDragEnd={onDragEnd}
          className="grid min-h-11 min-w-8 cursor-grab place-items-center rounded-lg text-ink-dim hover:bg-panel2 hover:text-ink active:cursor-grabbing focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40"
          aria-label={t("test.workflow.dragItem", { name: title })}
          title={t("test.workflow.dragHint")}
        >
          <GripVertical size={17} className="pointer-events-none" />
        </button>

        {groupable && (
          <button
            type="button"
            role="checkbox"
            aria-checked={groupSelected}
            aria-label={t("test.group.selectItem")}
            onClick={() => onToggleGroup(index)}
            className={`grid h-5 w-5 shrink-0 place-items-center rounded border text-[10px] transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40 ${
              groupSelected
                ? "border-brand bg-brand text-on-brand"
                : "border-line bg-panel2 text-transparent hover:border-brand/60"
            }`}
          >
            ✓
          </button>
        )}

        <button
          type="button"
          draggable={unit}
          onDragStart={unit ? (event) => onDragStart(event, index) : undefined}
          onDragEnd={unit ? onDragEnd : undefined}
          onClick={() => onSelect(index)}
          title={unit ? t("test.workflow.dragHint") : undefined}
          className={`flex min-w-0 flex-1 items-center gap-3 rounded-xl text-left focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40 ${
            unit ? "cursor-grab active:cursor-grabbing" : ""
          }`}
        >
          <span className={`grid h-9 w-9 shrink-0 place-items-center rounded-[10px] ${
            loop || condition ? "bg-violet-500/10 text-violet-500" : "bg-brand/10 text-brand"
          }`}>
            <Icon size={16} />
          </span>
          <span className="min-w-0 flex-1">
            <span className="flex items-center gap-2">
              <span className="truncate text-[13px] font-semibold text-ink">{title}</span>
              <Badge tone={loop || condition ? "brand" : "neutral"}>
                {condition ? "If / Else" : loop ? (unit ? "Unit" : t("test.loop.title")) : `Step ${index + 1}`}
              </Badge>
            </span>
            <span className="mt-1 block truncate font-mono text-[11px] text-ink-dim">
              {itemSummary(item, t)}
            </span>
          </span>
        </button>

        <span className="group/help relative">
          <button
            type="button"
            className="grid min-h-10 min-w-9 place-items-center rounded-lg text-ink-dim hover:bg-panel2 hover:text-brand focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40"
            aria-label={t("test.workflow.helpFor", { name: title })}
            title={t(`test.workflow.help.${condition ? "condition" : loop ? (unit ? "unit" : "loop") : item.type}`)}
          >
            <HelpCircle size={14} />
          </button>
          <span
            role="tooltip"
            className="pointer-events-none absolute bottom-full right-0 z-30 mb-2 hidden w-64 rounded-xl border border-line/70 bg-panel px-3 py-2 text-[11px] font-normal leading-5 text-ink shadow-xl group-hover/help:block group-focus-within/help:block"
          >
            {t(`test.workflow.help.${condition ? "condition" : loop ? (unit ? "unit" : "loop") : item.type}`)}
          </span>
        </span>

        <div className="flex items-center gap-0.5 opacity-70 transition-opacity group-hover:opacity-100 group-focus-within:opacity-100">
          <button
            type="button"
            onClick={() => onMove(index, -1)}
            disabled={index === 0}
            className="grid min-h-10 min-w-9 place-items-center rounded-lg text-ink-dim hover:bg-panel2 hover:text-ink disabled:opacity-25"
            aria-label={t("test.step.up")}
          >
            <ChevronUp size={15} />
          </button>
          <button
            type="button"
            onClick={() => onMove(index, 1)}
            disabled={index === total - 1}
            className="grid min-h-10 min-w-9 place-items-center rounded-lg text-ink-dim hover:bg-panel2 hover:text-ink disabled:opacity-25"
            aria-label={t("test.step.down")}
          >
            <ChevronDown size={15} />
          </button>
          <button
            type="button"
            onClick={() => onDuplicate(index)}
            className="grid min-h-10 min-w-9 place-items-center rounded-lg text-ink-dim hover:bg-panel2 hover:text-ink"
            aria-label={t("test.workflow.duplicate")}
          >
            <Copy size={14} />
          </button>
          <button
            type="button"
            onClick={() => onDelete(index)}
            className="grid min-h-10 min-w-9 place-items-center rounded-lg text-ink-dim hover:bg-danger/10 hover:text-danger"
            aria-label={t("test.step.delete")}
          >
            <Trash2 size={14} />
          </button>
        </div>
      </div>
      {(loop || condition) && (
        <FlowStructurePreview
          item={item as TestLoop | TestCondition}
          drag={drag}
          onNestedDragEnter={onNestedDragEnter}
          onDropItem={(event, branch) => onDropNestedItem(event, item.id, branch)}
          onRemoveItem={(branch, nestedItemId) => onRemoveNestedItem(item.id, branch, nestedItemId)}
        />
      )}
    </article>
  );
});
