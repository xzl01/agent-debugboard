import { GitFork, Package, Plus, Repeat2, Sparkles, Trash2 } from "lucide-react";
import { Badge } from "../ui";
import type { CustomUnitTemplate, DragAction, StepType } from "./types";
import { PALETTE_GROUPS, STEP_ICONS, setDragPayload } from "./utils";
import { MAX_EXECUTION_STEPS } from "@/lib/testScript";
import { useI18n } from "@/lib/i18n";

export interface PaletteProps {
  executionCount: number;
  customUnits: CustomUnitTemplate[];
  dragDispatch: React.Dispatch<DragAction>;
  onAddStep: (type: StepType) => void;
  onAddLoop: () => void;
  onAddCondition: () => void;
  onAddCustomUnit: (unit: CustomUnitTemplate) => void;
  onDeleteCustomUnit: (id: string) => void;
  onShowGroupHint: () => void;
}

export function Palette({
  executionCount,
  customUnits,
  dragDispatch,
  onAddStep,
  onAddLoop,
  onAddCondition,
  onAddCustomUnit,
  onDeleteCustomUnit,
  onShowGroupHint,
}: PaletteProps) {
  const { t } = useI18n();
  const atLimit = executionCount >= MAX_EXECUTION_STEPS;

  return (
    <aside className="border-b border-line/60 bg-panel2/25 p-3 xl:min-h-0 xl:overflow-x-hidden xl:overflow-y-auto xl:overscroll-contain xl:border-b-0 xl:border-r" aria-label={t("test.workflow.library")}>
      <div className="mb-3 flex items-center gap-2">
        <span className="grid h-8 w-8 place-items-center rounded-lg bg-brand/10 text-brand">
          <Sparkles size={15} />
        </span>
        <div>
          <h3 className="text-xs font-semibold text-ink">{t("test.workflow.library")}</h3>
          <p className="text-[10px] text-ink-dim">{t("test.workflow.libraryHint")}</p>
        </div>
      </div>
      <div className="grid gap-3 sm:grid-cols-2 md:grid-cols-4 xl:grid-cols-1">
        {PALETTE_GROUPS.map((group) => (
          <section key={group.key}>
            <h4 className="mb-1.5 px-1 text-[10px] font-semibold uppercase tracking-wider text-ink-dim">
              {t(`test.workflow.group.${group.key}`)}
            </h4>
            <div className="space-y-1">
              {group.types.map((type) => {
                const Icon = STEP_ICONS[type];
                return (
                  <button
                    key={type}
                    type="button"
                    draggable
                    onDragStart={(event) => {
                      const payload = { kind: "palette" as const, stepType: type };
                      setDragPayload(event, payload);
                      dragDispatch({ type: "start", payload });
                    }}
                    onDragEnd={() => dragDispatch({ type: "end" })}
                    onClick={() => onAddStep(type)}
                    disabled={atLimit}
                    className="flex min-h-11 w-full cursor-grab items-center gap-2 rounded-xl border border-transparent px-2.5 py-2 text-left text-xs font-medium text-ink transition-colors hover:border-line/70 hover:bg-panel active:cursor-grabbing disabled:cursor-not-allowed disabled:opacity-40"
                  >
                    <span className="grid h-7 w-7 shrink-0 place-items-center rounded-lg bg-brand/10 text-brand">
                      <Icon size={14} />
                    </span>
                    <span className="min-w-0 flex-1 truncate">{t(`test.step.${type}`)}</span>
                    <Plus size={13} className="text-ink-dim" />
                  </button>
                );
              })}
            </div>
          </section>
        ))}

        {/* Flow control section */}
        <section>
          <h4 className="mb-1.5 px-1 text-[10px] font-semibold uppercase tracking-wider text-ink-dim">
            {t("test.workflow.group.flow")}
          </h4>
          <div className="space-y-1">
            <button
              type="button"
              onClick={onAddLoop}
              className="flex min-h-11 w-full items-center gap-2 rounded-xl border border-transparent px-2.5 py-2 text-left text-xs font-medium text-ink transition-colors hover:border-line/70 hover:bg-panel"
            >
              <span className="grid h-7 w-7 place-items-center rounded-lg bg-violet-500/10 text-violet-500">
                <Repeat2 size={14} />
              </span>
              <span className="min-w-0 flex-1 truncate">{t("test.loop.title")}</span>
              <Plus size={13} className="text-ink-dim" />
            </button>
            <button
              type="button"
              onClick={onAddCondition}
              className="flex min-h-11 w-full items-center gap-2 rounded-xl border border-transparent px-2.5 py-2 text-left text-xs font-medium text-ink transition-colors hover:border-line/70 hover:bg-panel focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40"
            >
              <span className="grid h-7 w-7 place-items-center rounded-lg bg-violet-500/10 text-violet-500">
                <GitFork size={14} />
              </span>
              <span className="min-w-0 flex-1 truncate">{t("test.condition.title")}</span>
              <Plus size={13} className="text-ink-dim" />
            </button>
            <button
              type="button"
              onClick={onShowGroupHint}
              className="flex min-h-11 w-full items-center gap-2 rounded-xl border border-transparent px-2.5 py-2 text-left text-xs font-medium text-ink transition-colors hover:border-line/70 hover:bg-panel"
            >
              <span className="grid h-7 w-7 place-items-center rounded-lg bg-violet-500/10 text-violet-500">
                <Package size={14} />
              </span>
              <span className="min-w-0 flex-1 truncate">{t("test.workflow.packageUnit")}</span>
            </button>
          </div>
        </section>

        {/* Custom units section */}
        {customUnits.length > 0 && (
          <section>
            <h4 className="mb-1.5 px-1 text-[10px] font-semibold uppercase tracking-wider text-ink-dim">
              {t("test.workflow.customUnits")}
            </h4>
            <div className="space-y-1">
              {customUnits.map((unit) => (
                <div key={unit.id} className="group/unit flex items-center rounded-xl hover:bg-panel">
                  <button
                    type="button"
                    draggable
                    onDragStart={(event) => {
                      const payload = { kind: "unit" as const, templateId: unit.id };
                      setDragPayload(event, payload);
                      dragDispatch({ type: "start", payload });
                    }}
                    onDragEnd={() => dragDispatch({ type: "end" })}
                    onClick={() => onAddCustomUnit(unit)}
                    className="flex min-h-11 min-w-0 flex-1 cursor-grab items-center gap-2 px-2.5 py-2 text-left text-xs font-medium text-ink active:cursor-grabbing"
                  >
                    <span className="grid h-7 w-7 shrink-0 place-items-center rounded-lg bg-violet-500/10 text-violet-500">
                      <Package size={14} />
                    </span>
                    <span className="min-w-0 flex-1 truncate">{unit.name}</span>
                    <Badge tone="neutral">{unit.steps.length}</Badge>
                  </button>
                  <button
                    type="button"
                    onClick={() => onDeleteCustomUnit(unit.id)}
                    className="grid min-h-10 min-w-9 place-items-center rounded-lg text-ink-dim opacity-0 hover:bg-danger/10 hover:text-danger group-hover/unit:opacity-100 focus:opacity-100"
                    aria-label={t("test.workflow.deleteUnit", { name: unit.name })}
                  >
                    <Trash2 size={13} />
                  </button>
                </div>
              ))}
            </div>
          </section>
        )}
      </div>
    </aside>
  );
}
