import { useEffect, useState } from "react";
import { CheckCircle2, ChevronDown, Copy, Package, Plus, Settings2, Trash2, Ungroup } from "lucide-react";
import { Badge, Button } from "../ui";
import { TestStepInspector } from "../TestEditor";
import type {
  ConditionCheckType,
  CustomUnitTemplate,
  StepType,
  TestCondition,
  TestLoop,
  TestNestedItem,
  TestScriptItem,
  TestStep,
} from "./types";
import { inputClass } from "./types";
import { CONDITION_CHECK_TYPES, STEP_ICONS, createConditionCheck, unitFromTemplate } from "./utils";
import {
  MAX_LOOP_COUNT,
  MIN_LOOP_COUNT,
  STEP_TYPES,
  compatibleAssertionForStepType,
  defaultStepParams,
  generateStepId,
  isTestCondition,
  isTestLoop,
  isTestUnit,
} from "@/lib/testScript";
import { useI18n } from "@/lib/i18n";

// ─── Unit picker ───────────────────────────────────────────────────────────────

function UnitPicker({
  units,
  onAdd,
}: {
  units: CustomUnitTemplate[];
  onAdd: (unit: CustomUnitTemplate) => void;
}) {
  const { t } = useI18n();
  const [selectedUnitId, setSelectedUnitId] = useState(units[0]?.id ?? "");
  const selected = units.find((unit) => unit.id === selectedUnitId) ?? units[0];

  useEffect(() => {
    if (!units.some((unit) => unit.id === selectedUnitId)) setSelectedUnitId(units[0]?.id ?? "");
  }, [selectedUnitId, units]);

  if (units.length === 0) {
    return <p className="text-[10px] leading-5 text-ink-dim">{t("test.workflow.createUnitFirst")}</p>;
  }

  return (
    <div className="flex gap-2">
      <select
        className={`${inputClass} min-w-0 flex-1`}
        value={selected?.id ?? ""}
        onChange={(event) => setSelectedUnitId(event.target.value)}
        aria-label={t("test.workflow.customUnits")}
      >
        {units.map((unit) => <option key={unit.id} value={unit.id}>{unit.name}</option>)}
      </select>
      <Button variant="default" className="shrink-0" disabled={!selected} onClick={() => selected && onAdd(selected)}>
        <Package size={14} />
        {t("test.workflow.addUnit")}
      </Button>
    </div>
  );
}

// ─── Group (Loop/Unit) inspector ───────────────────────────────────────────────

function GroupInspector({
  group,
  onChange,
  customUnits,
}: {
  group: TestLoop;
  onChange: (group: TestLoop) => void;
  customUnits: CustomUnitTemplate[];
}) {
  const { t } = useI18n();
  const unit = isTestUnit(group);
  const updateChild = (index: number, child: TestNestedItem) => {
    const steps = [...group.params.steps];
    steps[index] = child;
    onChange({ ...group, params: { ...group.params, steps } });
  };

  return (
    <div className="space-y-4">
      {unit ? (
        <label className="block space-y-1.5">
          <span className="text-xs font-medium text-ink-dim">{t("test.unit.name")}</span>
          <input
            className={inputClass}
            value={group.params.unit.name}
            maxLength={80}
            onChange={(event) => onChange({
              ...group,
              params: { ...group.params, unit: { name: event.target.value } },
            })}
          />
        </label>
      ) : (
        <label className="block space-y-1.5">
          <span className="text-xs font-medium text-ink-dim">{t("test.loop.rounds")}</span>
          <input
            type="number"
            min={MIN_LOOP_COUNT}
            max={MAX_LOOP_COUNT}
            className={inputClass}
            value={group.params.count}
            onChange={(event) => onChange({
              ...group,
              params: {
                ...group.params,
                count: Math.min(
                  MAX_LOOP_COUNT,
                  Math.max(MIN_LOOP_COUNT, Math.trunc(Number(event.target.value) || MIN_LOOP_COUNT)),
                ),
              },
            })}
          />
        </label>
      )}

      <div>
        <div className="mb-2 text-xs font-medium text-ink-dim">
          {t("test.workflow.containedSteps", { n: group.params.steps.length })}
        </div>
        <div className="space-y-2">
          {group.params.steps.map((child, index) => {
            const childUnit = isTestLoop(child) ? child : null;
            const Icon = childUnit ? Package : STEP_ICONS[(child as TestStep).type];
            return (
              <details key={child.id} className="group rounded-xl border border-line/60 bg-panel2/35">
                <summary className="flex min-h-11 cursor-pointer list-none items-center gap-2 px-3 py-2 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-inset focus-visible:ring-brand/40">
                  <Icon size={14} className={childUnit ? "text-violet-500" : "text-brand"} />
                  <span className="min-w-0 flex-1 truncate text-xs font-medium text-ink">
                    {index + 1}. {childUnit ? childUnit.params.unit?.name ?? t("test.unit.defaultName") : t(`test.step.${(child as TestStep).type}`)}
                  </span>
                  <ChevronDown size={14} className="text-ink-dim transition-transform group-open:rotate-180" />
                </summary>
                <div className="space-y-2 border-t border-line/50 p-3">
                  {childUnit ? (
                    <GroupInspector
                      group={childUnit}
                      customUnits={[]}
                      onChange={(value) => updateChild(index, value)}
                    />
                  ) : (
                    <TestStepInspector step={child as TestStep} onChange={(value) => updateChild(index, value)} />
                  )}
                </div>
              </details>
            );
          })}
        </div>
      </div>
      {!unit && (
        <section className="space-y-2 border-t border-line/60 pt-3">
          <div className="text-xs font-medium text-ink-dim">{t("test.workflow.addUnitToBody")}</div>
          <UnitPicker
            units={customUnits}
            onAdd={(template) => onChange({
              ...group,
              params: { ...group.params, steps: [...group.params.steps, unitFromTemplate(template)] },
            })}
          />
        </section>
      )}
    </div>
  );
}

// ─── Step collection editor (condition branches) ───────────────────────────────

function StepCollectionEditor({
  label,
  tone,
  steps,
  onChange,
  customUnits,
}: {
  label: string;
  tone: "then" | "else";
  steps: TestNestedItem[];
  onChange: (steps: TestNestedItem[]) => void;
  customUnits: CustomUnitTemplate[];
}) {
  const { t } = useI18n();
  const [addType, setAddType] = useState<StepType>("delay");

  const updateStep = (index: number, step: TestNestedItem) => {
    const next = [...steps];
    next[index] = step;
    onChange(next);
  };
  const moveStep = (from: number, to: number) => {
    if (to < 0 || to >= steps.length) return;
    const next = [...steps];
    const [step] = next.splice(from, 1);
    next.splice(to, 0, step);
    onChange(next);
  };

  return (
    <section className={`rounded-xl border p-3 ${
      tone === "then"
        ? "border-ok/30 bg-ok/[0.045]"
        : "border-warn/30 bg-warn/[0.045]"
    }`}>
      <div className="mb-2 flex items-center gap-2">
        <span className={`h-2 w-2 rounded-full ${tone === "then" ? "bg-ok" : "bg-warn"}`} />
        <h4 className="text-xs font-semibold text-ink">{label}</h4>
        <Badge tone="neutral">{steps.length}</Badge>
      </div>

      <div className="space-y-2">
        {steps.map((step, index) => {
          const unit = isTestLoop(step) ? step : null;
          const Icon = unit ? Package : STEP_ICONS[(step as TestStep).type];
          return (
            <details key={step.id} className="group rounded-xl border border-line/60 bg-panel/75">
              <summary className="flex min-h-11 cursor-pointer list-none items-center gap-2 px-2.5 py-2 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-inset focus-visible:ring-brand/40">
                <Icon size={14} className={`shrink-0 ${unit ? "text-violet-500" : "text-brand"}`} />
                <span className="min-w-0 flex-1 truncate text-xs font-medium text-ink">
                  {index + 1}. {unit ? unit.params.unit?.name ?? t("test.unit.defaultName") : t(`test.step.${(step as TestStep).type}`)}
                </span>
                <button
                  type="button"
                  onClick={(event) => { event.preventDefault(); moveStep(index, index - 1); }}
                  disabled={index === 0}
                  className="grid min-h-9 min-w-8 place-items-center rounded-lg text-ink-dim hover:bg-panel2 disabled:opacity-25"
                  aria-label={t("test.step.up")}
                >
                  <ChevronDown size={13} className="rotate-180" />
                </button>
                <button
                  type="button"
                  onClick={(event) => { event.preventDefault(); moveStep(index, index + 1); }}
                  disabled={index === steps.length - 1}
                  className="grid min-h-9 min-w-8 place-items-center rounded-lg text-ink-dim hover:bg-panel2 disabled:opacity-25"
                  aria-label={t("test.step.down")}
                >
                  <ChevronDown size={13} />
                </button>
                <button
                  type="button"
                  onClick={(event) => {
                    event.preventDefault();
                    onChange(steps.filter((_, current) => current !== index));
                  }}
                  className="grid min-h-9 min-w-8 place-items-center rounded-lg text-ink-dim hover:bg-danger/10 hover:text-danger"
                  aria-label={t("test.step.delete")}
                >
                  <Trash2 size={13} />
                </button>
                <ChevronDown size={14} className="text-ink-dim transition-transform group-open:rotate-180" />
              </summary>
              <div className="space-y-3 border-t border-line/50 p-3">
                {unit ? (
                  <GroupInspector group={unit} customUnits={[]} onChange={(value) => updateStep(index, value)} />
                ) : (
                  <>
                    <label className="block space-y-1.5">
                      <span className="text-xs font-medium text-ink-dim">{t("test.condition.stepType")}</span>
                      <select
                        className={inputClass}
                        value={(step as TestStep).type}
                        onChange={(event) => {
                          const type = event.target.value as StepType;
                          const current = step as TestStep;
                          updateStep(index, {
                            ...current,
                            type,
                            params: defaultStepParams(type),
                            assert: compatibleAssertionForStepType(current.assert, type),
                          });
                        }}
                      >
                        {STEP_TYPES.map((type) => (
                          <option key={type} value={type}>{t(`test.step.${type}`)}</option>
                        ))}
                      </select>
                    </label>
                    <TestStepInspector step={step as TestStep} onChange={(value) => updateStep(index, value)} />
                  </>
                )}
              </div>
            </details>
          );
        })}
      </div>

      <div className="mt-2 flex gap-2">
        <select
          className={`${inputClass} min-w-0 flex-1`}
          value={addType}
          onChange={(event) => setAddType(event.target.value as StepType)}
          aria-label={t("test.condition.stepType")}
        >
          {STEP_TYPES.map((type) => (
            <option key={type} value={type}>{t(`test.step.${type}`)}</option>
          ))}
        </select>
        <Button
          variant="default"
          className="shrink-0"
          onClick={() => onChange([
            ...steps,
            { id: generateStepId(), type: addType, params: defaultStepParams(addType) },
          ])}
        >
          <Plus size={14} />
          {t("test.condition.addStep")}
        </Button>
      </div>
      <div className="mt-2 border-t border-line/50 pt-2">
        <UnitPicker units={customUnits} onAdd={(template) => onChange([...steps, unitFromTemplate(template)])} />
      </div>
    </section>
  );
}

// ─── Condition inspector ───────────────────────────────────────────────────────

function ConditionInspector({
  condition,
  onChange,
  customUnits,
}: {
  condition: TestCondition;
  onChange: (condition: TestCondition) => void;
  customUnits: CustomUnitTemplate[];
}) {
  const { t } = useI18n();
  const updateParams = (params: Partial<TestCondition["params"]>) => {
    onChange({ ...condition, params: { ...condition.params, ...params } });
  };

  return (
    <div className="space-y-4">
      <section className="rounded-xl border border-brand/30 bg-brand/[0.045] p-3">
        <div className="mb-3 flex items-center gap-2">
          <CheckCircle2 size={15} className="text-brand" />
          <h4 className="text-xs font-semibold text-ink">{t("test.condition.check")}</h4>
        </div>
        <label className="block space-y-1.5">
          <span className="text-xs font-medium text-ink-dim">{t("test.condition.checkType")}</span>
          <select
            className={inputClass}
            value={condition.params.check.type}
            onChange={(event) => {
              const type = event.target.value as ConditionCheckType;
              const next = createConditionCheck(type);
              updateParams({
                check: {
                  ...next,
                  id: condition.params.check.id,
                  continue_on_error: condition.params.check.continue_on_error,
                  assert: compatibleAssertionForStepType(condition.params.check.assert, type),
                },
              });
            }}
          >
            {CONDITION_CHECK_TYPES.map((type) => (
              <option key={type} value={type}>{t(`test.step.${type}`)}</option>
            ))}
          </select>
        </label>
        <div className="mt-3">
          <TestStepInspector
            step={condition.params.check}
            onChange={(check) => updateParams({ check: check as TestStep<ConditionCheckType> })}
          />
        </div>
        <p className="mt-3 text-[10px] leading-5 text-ink-dim">
          {t("test.condition.checkHint")}
        </p>
      </section>

      <StepCollectionEditor
        label={t("test.condition.then")}
        tone="then"
        steps={condition.params.then_steps}
        customUnits={customUnits}
        onChange={(then_steps) => updateParams({ then_steps })}
      />
      <StepCollectionEditor
        label={t("test.condition.else")}
        tone="else"
        steps={condition.params.else_steps}
        customUnits={customUnits}
        onChange={(else_steps) => updateParams({ else_steps })}
      />
    </div>
  );
}

// ─── Main inspector panel ──────────────────────────────────────────────────────

export interface InspectorPanelProps {
  selectedItem: TestScriptItem | undefined;
  selectedIndex: number;
  customUnits: CustomUnitTemplate[];
  onUpdateItem: (index: number, item: TestScriptItem) => void;
  onDuplicate: (index: number) => void;
  onDelete: (index: number) => void;
  onUngroup: () => void;
}

export function InspectorPanel({
  selectedItem,
  selectedIndex,
  customUnits,
  onUpdateItem,
  onDuplicate,
  onDelete,
  onUngroup,
}: InspectorPanelProps) {
  const { t } = useI18n();

  return (
    <aside className="border-t border-line/60 bg-panel p-4 xl:min-h-0 xl:overflow-x-hidden xl:overflow-y-auto xl:overscroll-contain xl:border-l xl:border-t-0" aria-label={t("test.workflow.inspector")}>
      <div className="mb-4 flex items-center gap-2">
        <span className="grid h-8 w-8 place-items-center rounded-lg bg-panel2 text-ink-dim">
          <Settings2 size={15} />
        </span>
        <div>
          <h3 className="text-xs font-semibold text-ink">{t("test.workflow.inspector")}</h3>
          <p className="text-[10px] text-ink-dim">{t("test.workflow.inspectorHint")}</p>
        </div>
      </div>

      {selectedItem ? (
        <div className="space-y-4">
          <div className="rounded-xl border border-line/60 bg-panel2/35 px-3 py-2.5">
            <div className="text-[10px] font-semibold uppercase tracking-wider text-ink-dim">
              {isTestCondition(selectedItem)
                ? t("test.condition.title")
                : isTestLoop(selectedItem)
                ? isTestUnit(selectedItem) ? t("test.unit.name") : t("test.loop.title")
                : `#${selectedIndex + 1}`}
            </div>
            <div className="mt-1 text-sm font-semibold text-ink">
              {isTestCondition(selectedItem)
                ? t("test.condition.title")
                : isTestLoop(selectedItem)
                ? isTestUnit(selectedItem) ? selectedItem.params.unit.name : t("test.loop.title")
                : t(`test.step.${selectedItem.type}`)}
            </div>
            <div className="mt-1 break-words font-mono text-[10px] text-ink-dim">
              {selectedItem.id}
            </div>
          </div>

          {isTestCondition(selectedItem) ? (
            <ConditionInspector
              condition={selectedItem}
              customUnits={customUnits}
              onChange={(value) => onUpdateItem(selectedIndex, value)}
            />
          ) : isTestLoop(selectedItem) ? (
            <GroupInspector
              group={selectedItem}
              customUnits={customUnits}
              onChange={(value) => onUpdateItem(selectedIndex, value)}
            />
          ) : (
            <>
              <TestStepInspector
                step={selectedItem}
                onChange={(value) => onUpdateItem(selectedIndex, value)}
              />
              <label className="flex min-h-11 items-center justify-between gap-3 rounded-xl border border-line/60 bg-panel2/35 px-3 py-2 text-xs text-ink">
                <span>{t("test.workflow.continueOnError")}</span>
                <input
                  type="checkbox"
                  checked={Boolean(selectedItem.continue_on_error)}
                  onChange={(event) => onUpdateItem(selectedIndex, {
                    ...selectedItem,
                    continue_on_error: event.target.checked || undefined,
                  })}
                  className="h-4 w-4 accent-brand"
                />
              </label>
            </>
          )}

          <div className="grid grid-cols-2 gap-2 border-t border-line/60 pt-4">
            <Button variant="default" onClick={() => onDuplicate(selectedIndex)}>
              <Copy size={14} />
              {t("test.workflow.duplicate")}
            </Button>
            <Button variant="danger" onClick={() => onDelete(selectedIndex)}>
              <Trash2 size={14} />
              {t("test.workflow.delete")}
            </Button>
            {isTestLoop(selectedItem) && (
              <Button variant="ghost" className="col-span-2" onClick={onUngroup}>
                <Ungroup size={14} />
                {isTestUnit(selectedItem) ? t("test.unit.ungroup") : t("test.loop.ungroup")}
              </Button>
            )}
          </div>
        </div>
      ) : (
        <div className="rounded-2xl border border-dashed border-line/70 px-4 py-10 text-center">
          <Settings2 size={20} className="mx-auto text-ink-dim" />
          <p className="mt-2 text-xs font-medium text-ink">{t("test.workflow.noSelection")}</p>
          <p className="mt-1 text-[10px] leading-5 text-ink-dim">{t("test.workflow.noSelectionHint")}</p>
        </div>
      )}
    </aside>
  );
}
