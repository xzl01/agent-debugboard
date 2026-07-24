import { memo, useCallback, useMemo, useRef, useState } from "react";
import {
  ChevronDown,
  ChevronUp,
  Download,
  FileUp,
  Play,
  Plus,
  Trash2,
  Zap,
  Power,
  Timer,
  Hourglass,
  Send,
  MessageSquareText,
  Package,
  Repeat2,
  SquareCheckBig,
  Activity,
  ToggleRight,
  CheckCircle,
  GitBranch,
  Ungroup,
  type LucideIcon,
} from "lucide-react";
import { Badge, Button } from "./ui";
import type {
  TestLoop,
  TestNestedItem,
  TestScript,
  TestScriptItem,
  TestStep,
  StepType,
  StepAssertion,
} from "@/lib/testScript";
import {
  MAX_LOOP_COUNT,
  MAX_EXECUTION_STEPS,
  MIN_LOOP_COUNT,
  STEP_TYPES,
  buildExecutionPlan,
  countScriptCommands,
  stepTypeIcon,
  defaultStepParams,
  generateLoopId,
  generateStepId,
  isTestCondition,
  isTestLoop,
  isTestUnit,
  parseTestScript,
  serializeTestScript,
} from "@/lib/testScript";
import { POWER_SWITCH_RAILS, USER_POWER_RAILS } from "@/lib/power";
import { downloadBlob } from "@/lib/utils";
import { useI18n } from "@/lib/i18n";

const ICON_MAP: Record<string, LucideIcon> = {
  Zap, Power, Timer, Hourglass, Send, MessageSquareText, Activity,
  ToggleRight, CheckCircle, GitBranch,
};

function iconFor(type: StepType): LucideIcon {
  return ICON_MAP[stepTypeIcon(type)] ?? Zap;
}

export function StepParams({
  step,
  onParamChange,
}: {
  step: TestStep;
  onParamChange: (key: string, value: unknown) => void;
}) {
  const { t } = useI18n();
  switch (step.type) {
    case "power_on":
    case "power_off": {
      const p = step.params as import("@/lib/testScript").PowerOnParams;
      return (
        <ParamRow label={t("test.param.rail")}>
          <select className={inputCls} value={p.rail} onChange={(e) => onParamChange("rail", e.target.value)}>
            {POWER_SWITCH_RAILS.map((r) => <option key={r} value={r}>{r}</option>)}
          </select>
        </ParamRow>
      );
    }
    case "delay": {
      const p = step.params as import("@/lib/testScript").DelayParams;
      return (
        <ParamRow label={t("test.param.ms")}>
          <input type="number" className={inputCls} value={p.ms} onChange={(e) => onParamChange("ms", Number(e.target.value))} />
        </ParamRow>
      );
    }
    case "serial_wait": {
      const p = step.params as import("@/lib/testScript").SerialWaitParams;
      return (
        <>
          <ParamRow label={t("test.param.channel")}>
            <select className={inputCls} value={p.channel} onChange={(e) => onParamChange("channel", e.target.value)}>
              <option value="uart0">{t("test.opt.uart0")}</option>
              <option value="uart1">{t("test.opt.uart1")}</option>
            </select>
          </ParamRow>
          <ParamRow label={t("test.param.pattern")}>
            <input className={inputCls} value={p.pattern} onChange={(e) => onParamChange("pattern", e.target.value)} />
          </ParamRow>
          <ParamRow label={t("test.param.timeout")}>
            <input type="number" className={inputCls} value={p.timeout_ms} onChange={(e) => onParamChange("timeout_ms", Number(e.target.value))} />
          </ParamRow>
        </>
      );
    }
    case "serial_send": {
      const p = step.params as import("@/lib/testScript").SerialSendParams;
      return (
        <>
          <ParamRow label={t("test.param.channel")}>
            <select className={inputCls} value={p.channel} onChange={(e) => onParamChange("channel", e.target.value)}>
              <option value="uart0">{t("test.opt.uart0")}</option>
              <option value="uart1">{t("test.opt.uart1")}</option>
            </select>
          </ParamRow>
          <ParamRow label={t("test.param.text")}>
            <input className={inputCls} value={p.text} onChange={(e) => onParamChange("text", e.target.value)} />
          </ParamRow>
        </>
      );
    }
    case "serial_expect": {
      const p = step.params as import("@/lib/testScript").SerialExpectParams;
      return (
        <>
          <ParamRow label={t("test.param.channel")}>
            <select className={inputCls} value={p.channel} onChange={(e) => onParamChange("channel", e.target.value)}>
              <option value="uart0">{t("test.opt.uart0")}</option>
              <option value="uart1">{t("test.opt.uart1")}</option>
            </select>
          </ParamRow>
          <ParamRow label={t("test.param.command")}>
            <input className={inputCls} value={p.command} onChange={(e) => onParamChange("command", e.target.value)} />
          </ParamRow>
          <ParamRow label={t("test.param.pattern")}>
            <input className={inputCls} value={p.pattern} onChange={(e) => onParamChange("pattern", e.target.value)} />
          </ParamRow>
          <ParamRow label={t("test.param.timeout")}>
            <input type="number" className={inputCls} value={p.timeout_ms} onChange={(e) => onParamChange("timeout_ms", Number(e.target.value))} />
          </ParamRow>
        </>
      );
    }
    case "adc_read": {
      const p = step.params as import("@/lib/testScript").AdcReadParams;
      return (
        <ParamRow label={t("test.param.channel")}>
          <select className={inputCls} value={p.channel} onChange={(e) => onParamChange("channel", e.target.value)}>
            {USER_POWER_RAILS.map((r) => <option key={r} value={r}>{r}</option>)}
          </select>
        </ParamRow>
      );
    }
    case "gpio_set": {
      const p = step.params as import("@/lib/testScript").GpioSetParams;
      return (
        <>
          <ParamRow label={t("test.param.pin")}>
            <input className={inputCls} value={p.pin} onChange={(e) => onParamChange("pin", e.target.value)} />
          </ParamRow>
          <ParamRow label={t("test.param.value")}>
            <select className={inputCls} value={p.value} onChange={(e) => onParamChange("value", Number(e.target.value))}>
              <option value="1">{t("test.opt.high")}</option>
              <option value="0">{t("test.opt.low")}</option>
            </select>
          </ParamRow>
        </>
      );
    }
    case "gpio_assert": {
      const p = step.params as import("@/lib/testScript").GpioAssertParams;
      return (
        <>
          <ParamRow label={t("test.param.pin")}>
            <input className={inputCls} value={p.pin} onChange={(e) => onParamChange("pin", e.target.value)} />
          </ParamRow>
          <ParamRow label={t("test.param.direction")}>
            <select className={inputCls} value={p.direction} onChange={(e) => onParamChange("direction", e.target.value)}>
              <option value="input">{t("test.opt.input")}</option>
              <option value="output">{t("test.opt.output")}</option>
            </select>
          </ParamRow>
          <ParamRow label={t("test.param.value")}>
            <select className={inputCls} value={p.value} onChange={(e) => onParamChange("value", Number(e.target.value))}>
              <option value="1">1</option>
              <option value="0">0</option>
            </select>
          </ParamRow>
        </>
      );
    }
    case "switch_route": {
      const p = step.params as import("@/lib/testScript").SwitchRouteParams;
      return (
        <>
          <ParamRow label={t("test.param.switch")}>
            <select className={inputCls} value={p.switch} onChange={(e) => onParamChange("switch", e.target.value)}>
              <option value="sd">{t("test.opt.sd")}</option>
              <option value="usb">{t("test.opt.usb")}</option>
              <option value="vin">{t("test.opt.vin")}</option>
            </select>
          </ParamRow>
          <ParamRow label={t("test.param.route")}>
            <input className={inputCls} value={p.route} onChange={(e) => onParamChange("route", e.target.value)} />
          </ParamRow>
        </>
      );
    }
    case "capture": {
      const p = step.params as import("@/lib/testScript").CaptureParams;
      return (
        <>
          <ParamRow label={t("test.param.rail")}>
            <select className={inputCls} value={p.rail} onChange={(e) => onParamChange("rail", e.target.value)}>
              {USER_POWER_RAILS.map((r) => <option key={r} value={r}>{r}</option>)}
            </select>
          </ParamRow>
          <ParamRow label={t("test.param.trigger")}>
            <select className={inputCls} value={p.trigger} onChange={(e) => onParamChange("trigger", e.target.value)}>
              <option value="manual">{t("test.param.manual")}</option>
              <option value="current">{t("test.param.currentThreshold")}</option>
              <option value="power_on">{t("test.param.powerOn")}</option>
            </select>
          </ParamRow>
          {p.trigger === "current" && (
            <ParamRow label={t("test.param.threshold")}>
              <input
                type="number"
                min="0"
                step="0.01"
                className={inputCls}
                value={p.threshold_a ?? 0.1}
                onChange={(e) => onParamChange("threshold_a", Number(e.target.value))}
              />
            </ParamRow>
          )}
          <ParamRow label={t("test.param.duration")}>
            <input type="number" className={inputCls} value={p.duration_ms} onChange={(e) => onParamChange("duration_ms", Number(e.target.value))} />
          </ParamRow>
        </>
      );
    }
  }
}

export function StepAssertions({
  step,
  assert,
  onAssertChange,
  onAssertRemove,
}: {
  step: TestStep;
  assert: StepAssertion | undefined;
  onAssertChange: (key: string, value: unknown) => void;
  onAssertRemove: (key: string) => void;
}) {
  const { t } = useI18n();
  const [invalidJson, setInvalidJson] = useState<Set<string>>(new Set());

  const toggleField = (key: string, defaultVal: unknown) => {
    if (assert && key in assert) onAssertRemove(key);
    else onAssertChange(key, defaultVal);
    setInvalidJson((prev) => { const next = new Set(prev); next.delete(key); return next; });
  };

  const availableFields: Array<{ key: string; label: string; default: unknown }> = [];
  if (step.type === "adc_read") availableFields.push({ key: "current_range", label: t("test.assert.currentRange"), default: { min_a: 0, max_a: 3 } });
  if (step.type === "serial_wait") availableFields.push({ key: "contains", label: t("test.assert.contains"), default: "" });
  if (step.type === "serial_expect") {
    availableFields.push({ key: "contains", label: t("test.assert.contains"), default: "" });
    availableFields.push({ key: "exit_code", label: t("test.assert.exitCode"), default: 0 });
  }
  if (step.type === "capture") {
    availableFields.push({ key: "peak_current_max_a", label: t("test.assert.peakCurrentMax"), default: 5 });
    availableFields.push({ key: "energy_max_j", label: t("test.assert.energyMax"), default: 10 });
  }

  if (availableFields.length === 0) return null;

  return (
    <div className="mt-2 border-t border-line/40 pt-2">
      <div className="mb-1 text-[10px] font-semibold uppercase tracking-wider text-ink-dim">{t("test.step.assert")}</div>
      {availableFields.map((f) => {
        const active = assert && f.key in assert;
        return (
          <div key={f.key} className="flex items-center gap-2 py-0.5">
            <button
              type="button"
              role="checkbox"
              aria-checked={active}
              onClick={() => toggleField(f.key, f.default)}
              className={`h-4 w-4 rounded border text-[10px] leading-none ${active ? "border-brand bg-brand text-white" : "border-line bg-panel2 text-transparent"}`}
            >
              {active ? "✓" : ""}
            </button>
            <span className="text-[11px] text-ink-dim">{f.label}</span>
            {active && (
              <input
                className={inputCls + (invalidJson.has(f.key) ? " border-danger" : "") + " flex-1"}
                value={typeof (assert as Record<string, unknown>)[f.key] === "object"
                  ? JSON.stringify((assert as Record<string, unknown>)[f.key])
                  : String((assert as Record<string, unknown>)[f.key] ?? "")}
                onChange={(e) => {
                  let val: unknown = e.target.value;
                  if (f.key === "exit_code" || f.key === "pin_value" || f.key === "peak_current_max_a" || f.key === "energy_max_j") val = Number(val);
                  if (f.key === "current_range") {
                    try {
                      val = JSON.parse(String(val));
                      setInvalidJson((prev) => { const next = new Set(prev); next.delete(f.key); return next; });
                    } catch {
                      setInvalidJson((prev) => new Set(prev).add(f.key));
                      return;
                    }
                  }
                  onAssertChange(f.key, val);
                }}
              />
            )}
          </div>
        );
      })}
    </div>
  );
}

const StepCard = memo(function StepCard({
  step,
  index,
  total,
  onChange,
  onDelete,
  onMoveUp,
  onMoveDown,
  selectable = false,
  selected = false,
  onSelect,
}: {
  step: TestStep;
  index: number;
  total: number;
  onChange: (s: TestStep) => void;
  onDelete: () => void;
  onMoveUp: () => void;
  onMoveDown: () => void;
  selectable?: boolean;
  selected?: boolean;
  onSelect?: () => void;
}) {
  const [expanded, setExpanded] = useState(false);
  const { t } = useI18n();
  const Icon = iconFor(step.type);

  return (
    <div className={`rounded-xl border px-3 py-2 transition-colors ${
      selected ? "border-brand/60 bg-brand/10" : "border-line/50 bg-panel2/30"
    }`}>
      <div className="flex items-center gap-2">
        {selectable && (
          <button
            type="button"
            role="checkbox"
            aria-checked={selected}
            aria-label={t("test.group.selectItem")}
            onClick={onSelect}
            className={`grid h-5 w-5 shrink-0 place-items-center rounded border text-[10px] transition-colors ${
              selected
                ? "border-brand bg-brand text-white"
                : "border-line bg-panel text-transparent hover:border-brand/60"
            }`}
          >
            ✓
          </button>
        )}
        <div className="flex flex-col gap-0.5">
          <button type="button" onClick={onMoveUp} disabled={index === 0} className="text-ink-dim hover:text-ink disabled:opacity-30" title={t("test.step.up")}>
            <ChevronUp size={12} />
          </button>
          <button type="button" onClick={onMoveDown} disabled={index === total - 1} className="text-ink-dim hover:text-ink disabled:opacity-30" title={t("test.step.down")}>
            <ChevronDown size={12} />
          </button>
        </div>
        <span className="grid h-6 w-6 place-items-center rounded-md bg-brand/10 text-brand">
          <Icon size={13} />
        </span>
        <span className="min-w-0 flex-1">
          <span className="text-xs font-semibold text-ink">{t(`test.step.${step.type}`)}</span>
          <span className="ml-2 truncate text-[11px] text-ink-dim">{testStepSummary(step)}</span>
        </span>
        <Badge tone="neutral">{index + 1}</Badge>
        <button
          type="button"
          onClick={() => setExpanded(!expanded)}
          className="text-ink-dim hover:text-ink"
        >
          <ChevronDown size={14} className={`transition-transform ${expanded ? "rotate-180" : ""}`} />
        </button>
        <button type="button" onClick={onDelete} className="text-ink-dim hover:text-danger" title={t("test.step.delete")}>
          <Trash2 size={13} />
        </button>
      </div>
      {expanded && (
        <div className="mt-2 space-y-1 pl-8">
          <TestStepInspector step={step} onChange={onChange} />
        </div>
      )}
    </div>
  );
});

export function testStepSummary(step: TestStep): string {
  switch (step.type) {
    case "power_on":
    case "power_off":
      return (step.params as import("@/lib/testScript").PowerOnParams).rail;
    case "delay":
      return `${(step.params as import("@/lib/testScript").DelayParams).ms}ms`;
    case "serial_wait": {
      const p = step.params as import("@/lib/testScript").SerialWaitParams;
      return `${p.channel} · ${p.pattern} · ${p.timeout_ms}ms`;
    }
    case "serial_send": {
      const p = step.params as import("@/lib/testScript").SerialSendParams;
      return `${p.channel} · ${p.text.replace(/\n/g, "\\n")}`;
    }
    case "serial_expect": {
      const p = step.params as import("@/lib/testScript").SerialExpectParams;
      return `${p.channel} · ${p.command}`;
    }
    case "adc_read":
      return (step.params as import("@/lib/testScript").AdcReadParams).channel;
    case "gpio_set": {
      const p = step.params as import("@/lib/testScript").GpioSetParams;
      return `${p.pin}=${p.value}`;
    }
    case "gpio_assert": {
      const p = step.params as import("@/lib/testScript").GpioAssertParams;
      return `${p.pin} · ${p.direction} · ${p.value}`;
    }
    case "switch_route": {
      const p = step.params as import("@/lib/testScript").SwitchRouteParams;
      return `${p.switch} → ${p.route}`;
    }
    case "capture": {
      const p = step.params as import("@/lib/testScript").CaptureParams;
      return `${p.rail} · ${p.trigger} · ${p.duration_ms}ms`;
    }
  }
}

export function TestStepInspector({
  step,
  onChange,
}: {
  step: TestStep;
  onChange: (step: TestStep) => void;
}) {
  const updateParam = (key: string, value: unknown) => {
    onChange({ ...step, params: { ...step.params, [key]: value } });
  };
  const updateAssert = (key: string, value: unknown) => {
    onChange({ ...step, assert: { ...(step.assert ?? {}), [key]: value } });
  };
  const removeAssert = (key: string) => {
    if (!step.assert) return;
    const next = { ...step.assert };
    delete (next as Record<string, unknown>)[key];
    onChange({ ...step, assert: Object.keys(next).length > 0 ? next : undefined });
  };

  return (
    <>
      <StepParams step={step} onParamChange={updateParam} />
      <StepAssertions
        step={step}
        assert={step.assert}
        onAssertChange={updateAssert}
        onAssertRemove={removeAssert}
      />
    </>
  );
}

const LoopCard = memo(function LoopCard({
  loop,
  index,
  total,
  onChange,
  onDelete,
  onUngroup,
  onMoveUp,
  onMoveDown,
  maxCount,
  selectable = false,
  selected = false,
  onSelect,
}: {
  loop: TestLoop;
  index: number;
  total: number;
  onChange: (loop: TestLoop) => void;
  onDelete: () => void;
  onUngroup: () => void;
  onMoveUp: () => void;
  onMoveDown: () => void;
  maxCount: number;
  selectable?: boolean;
  selected?: boolean;
  onSelect?: () => void;
}) {
  const { t } = useI18n();
  const unit = isTestUnit(loop);
  const [expanded, setExpanded] = useState(!unit);

  const updateChild = (childIndex: number, step: TestNestedItem) => {
    const steps = [...loop.params.steps];
    steps[childIndex] = step;
    onChange({ ...loop, params: { ...loop.params, steps } });
  };

  const deleteChild = (childIndex: number) => {
    if (loop.params.steps.length === 1) {
      onDelete();
      return;
    }
    onChange({
      ...loop,
      params: {
        ...loop.params,
        steps: loop.params.steps.filter((_, current) => current !== childIndex),
      },
    });
  };

  const moveChild = (from: number, to: number) => {
    if (to < 0 || to >= loop.params.steps.length) return;
    const steps = [...loop.params.steps];
    const [step] = steps.splice(from, 1);
    steps.splice(to, 0, step);
    onChange({ ...loop, params: { ...loop.params, steps } });
  };

  const executions = loop.params.steps.length * loop.params.count;

  return (
    <section
      className="rounded-xl border border-brand/40 bg-brand/[0.06] p-2"
      aria-label={unit ? loop.params.unit.name : t("test.loop.title")}
    >
      <div className="mb-2 flex flex-wrap items-center gap-2 px-1">
        {unit && selectable && (
          <button
            type="button"
            role="checkbox"
            aria-checked={selected}
            aria-label={t("test.group.selectItem")}
            onClick={onSelect}
            className={`grid h-5 w-5 shrink-0 place-items-center rounded border text-[10px] transition-colors ${
              selected
                ? "border-brand bg-brand text-white"
                : "border-line bg-panel text-transparent hover:border-brand/60"
            }`}
          >
            ✓
          </button>
        )}
        <div className="flex flex-col gap-0.5">
          <button type="button" onClick={onMoveUp} disabled={index === 0} className="text-ink-dim hover:text-ink disabled:opacity-30" title={t("test.step.up")}>
            <ChevronUp size={12} />
          </button>
          <button type="button" onClick={onMoveDown} disabled={index === total - 1} className="text-ink-dim hover:text-ink disabled:opacity-30" title={t("test.step.down")}>
            <ChevronDown size={12} />
          </button>
        </div>
        <span className="grid h-7 w-7 place-items-center rounded-lg bg-brand/15 text-brand">
          {unit ? <Package size={14} /> : <Repeat2 size={14} />}
        </span>
        <div className="min-w-0 flex-1">
          <div className="truncate text-xs font-semibold text-ink">
            {unit ? loop.params.unit.name : t("test.loop.title")}
          </div>
          <div className="text-[10px] text-ink-dim">
            {unit
              ? t("test.unit.summary", { commands: loop.params.steps.length })
              : t("test.loop.summary", { commands: loop.params.steps.length, executions })}
          </div>
        </div>
        {!unit && (
          <label className="flex items-center gap-1.5 text-[11px] text-ink-dim">
            <input
              type="number"
              min={MIN_LOOP_COUNT}
              max={maxCount}
              value={loop.params.count}
              onChange={(event) => {
                const count = Math.min(
                  maxCount,
                  Math.max(MIN_LOOP_COUNT, Math.trunc(Number(event.target.value) || MIN_LOOP_COUNT)),
                );
                onChange({ ...loop, params: { ...loop.params, count } });
              }}
              className={`${inputCls} w-20 text-center font-mono`}
              aria-label={t("test.loop.rounds")}
            />
            {t("test.loop.rounds")}
          </label>
        )}
        {unit && (
          <button
            type="button"
            onClick={() => setExpanded((value) => !value)}
            className="rounded-md p-1.5 text-ink-dim hover:bg-panel/70 hover:text-ink"
            aria-label={expanded ? t("test.unit.collapse") : t("test.unit.expand")}
            title={expanded ? t("test.unit.collapse") : t("test.unit.expand")}
          >
            <ChevronDown size={14} className={`transition-transform ${expanded ? "rotate-180" : ""}`} />
          </button>
        )}
        <button
          type="button"
          onClick={onUngroup}
          className="rounded-md p-1.5 text-ink-dim hover:bg-panel/70 hover:text-ink"
          title={unit ? t("test.unit.ungroup") : t("test.loop.ungroup")}
          aria-label={unit ? t("test.unit.ungroup") : t("test.loop.ungroup")}
        >
          <Ungroup size={14} />
        </button>
      </div>
      {(!unit || expanded) && <div className="space-y-1.5 border-l-2 border-brand/30 pl-2">
        {loop.params.steps.map((step, childIndex) => isTestLoop(step) ? (
          <LoopCard
            key={step.id}
            loop={step}
            index={childIndex}
            total={loop.params.steps.length}
            onChange={(value) => updateChild(childIndex, value)}
            onDelete={() => deleteChild(childIndex)}
            onUngroup={() => onChange({
              ...loop,
              params: {
                ...loop.params,
                steps: loop.params.steps.flatMap((entry, index) => index === childIndex ? step.params.steps : [entry]),
              },
            })}
            onMoveUp={() => moveChild(childIndex, childIndex - 1)}
            onMoveDown={() => moveChild(childIndex, childIndex + 1)}
            maxCount={1}
          />
        ) : (
          <StepCard
            key={step.id}
            step={step}
            index={childIndex}
            total={loop.params.steps.length}
            onChange={(value) => updateChild(childIndex, value)}
            onDelete={() => deleteChild(childIndex)}
            onMoveUp={() => moveChild(childIndex, childIndex - 1)}
            onMoveDown={() => moveChild(childIndex, childIndex + 1)}
          />
        ))}
      </div>}
    </section>
  );
});

const inputCls =
  "rounded-md border border-line/50 bg-panel px-2 py-1 text-xs text-ink outline-none focus-visible:ring-1 focus-visible:ring-brand/40 w-full";

function ParamRow({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex items-center gap-2 py-0.5">
      <span className="w-16 shrink-0 text-right text-[10px] text-ink-dim">{label}</span>
      <div className="min-w-0 flex-1">{children}</div>
    </div>
  );
}

export interface TestEditorProps {
  script: TestScript;
  onChange: (script: TestScript) => void;
  onRun: () => void;
  runDisabled?: boolean;
}

function flattenGroupItems(items: TestScriptItem[]): TestStep[] {
  const usedIds = new Set<string>();
  return items
    .flatMap((item) => isTestUnit(item)
      ? item.params.steps.filter((step): step is TestStep => !isTestLoop(step))
      : [item as TestStep])
    .map((step) => {
      let id = step.id;
      while (usedIds.has(id)) id = generateStepId();
      usedIds.add(id);
      return { ...step, id };
    });
}

export function TestEditor({ script, onChange, onRun, runDisabled = false }: TestEditorProps) {
  const { t } = useI18n();
  const fileRef = useRef<HTMLInputElement>(null);
  const [addType, setAddType] = useState<StepType>("power_on");
  const [importError, setImportError] = useState<string | null>(null);
  const [selectingLoop, setSelectingLoop] = useState(false);
  const [selectedStepIds, setSelectedStepIds] = useState<Set<string>>(new Set());
  const [unitName, setUnitName] = useState("");

  const commandCount = countScriptCommands(script);
  const executionCount = useMemo(() => {
    try {
      return buildExecutionPlan(script).length;
    } catch {
      return 0;
    }
  }, [script]);

  const selectedIndexes = useMemo(
    () => script.steps
      .map((item, index) => selectedStepIds.has(item.id) ? index : -1)
      .filter((index) => index >= 0),
    [script.steps, selectedStepIds],
  );
  const selectionIsContiguous = selectedIndexes.length > 0
    && selectedIndexes.every((index, offset) => index === selectedIndexes[0] + offset)
    && selectedIndexes.every((index) => !isTestLoop(script.steps[index]) || isTestUnit(script.steps[index]));

  const maxLoopCount = useCallback((loop: TestLoop) => {
    const otherExecutions = script.steps.reduce((total, item) => {
      if (item.id === loop.id) return total;
      return total + (isTestLoop(item) ? item.params.steps.length * item.params.count : 1);
    }, 0);
    return Math.max(
      MIN_LOOP_COUNT,
      Math.min(
        MAX_LOOP_COUNT,
        Math.floor((MAX_EXECUTION_STEPS - otherExecutions) / loop.params.steps.length),
      ),
    );
  }, [script.steps]);

  const updateItem = useCallback(
    (index: number, item: TestScriptItem) => {
      const steps = [...script.steps];
      steps[index] = item;
      onChange({ ...script, steps });
    },
    [script, onChange],
  );

  const deleteItem = useCallback(
    (index: number) => {
      const steps = script.steps.filter((_, i) => i !== index);
      onChange({ ...script, steps });
    },
    [script, onChange],
  );

  const moveItem = useCallback(
    (from: number, to: number) => {
      if (to < 0 || to >= script.steps.length) return;
      const steps = [...script.steps];
      const [item] = steps.splice(from, 1);
      steps.splice(to, 0, item);
      onChange({ ...script, steps });
    },
    [script, onChange],
  );

  const addStep = useCallback(() => {
    const step: TestStep = {
      id: generateStepId(),
      type: addType,
      params: defaultStepParams(addType),
    };
    onChange({ ...script, steps: [...script.steps, step] });
  }, [script, onChange, addType]);

  const cancelLoopSelection = useCallback(() => {
    setSelectingLoop(false);
    setSelectedStepIds(new Set());
    setUnitName("");
  }, []);

  const toggleLoopStep = useCallback((id: string) => {
    setSelectedStepIds((current) => {
      const next = new Set(current);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  }, []);

  const createLoop = useCallback(() => {
    if (!selectionIsContiguous) return;
    const firstIndex = selectedIndexes[0];
    const selectedItems = selectedIndexes.map((index) => script.steps[index]);
    if (selectedItems.some((item) => isTestLoop(item) && !isTestUnit(item))) return;
    const selectedSteps = flattenGroupItems(selectedItems);
    const otherExecutions = executionCount - selectedSteps.length;
    const count = Math.max(
      MIN_LOOP_COUNT,
      Math.min(2, Math.floor((MAX_EXECUTION_STEPS - otherExecutions) / selectedSteps.length)),
    );
    const loop: TestLoop = {
      id: generateLoopId(),
      type: "loop",
      params: { count, steps: selectedSteps },
    };
    const steps = [...script.steps];
    steps.splice(firstIndex, selectedItems.length, loop);
    onChange({ ...script, steps });
    cancelLoopSelection();
  }, [cancelLoopSelection, executionCount, onChange, script, selectedIndexes, selectionIsContiguous]);

  const createUnit = useCallback(() => {
    if (!selectionIsContiguous) return;
    const firstIndex = selectedIndexes[0];
    const selectedItems = selectedIndexes.map((index) => script.steps[index]);
    if (selectedItems.some((item) => isTestLoop(item) && !isTestUnit(item))) return;
    const unit: TestLoop = {
      id: generateLoopId(),
      type: "loop",
      params: {
        count: 1,
        steps: flattenGroupItems(selectedItems),
        unit: { name: unitName.trim() || t("test.unit.defaultName") },
      },
    };
    const steps = [...script.steps];
    steps.splice(firstIndex, selectedItems.length, unit);
    onChange({ ...script, steps });
    cancelLoopSelection();
  }, [cancelLoopSelection, onChange, script, selectedIndexes, selectionIsContiguous, t, unitName]);

  const ungroupLoop = useCallback((index: number, loop: TestLoop) => {
    const steps = [...script.steps];
    steps.splice(index, 1, ...loop.params.steps);
    onChange({ ...script, steps });
  }, [onChange, script]);

  const handleExport = useCallback(() => {
    const ndjson = serializeTestScript(script);
    downloadBlob(`${script.name.replace(/\s+/g, "-")}.ndjson`, ndjson, "application/x-ndjson");
  }, [script]);

  const handleImport = useCallback(
    (e: React.ChangeEvent<HTMLInputElement>) => {
      const file = e.target.files?.[0];
      if (!file) return;
      const reader = new FileReader();
      reader.onload = () => {
        try {
          const imported = parseTestScript(String(reader.result));
          setImportError(null);
          cancelLoopSelection();
          onChange(imported);
        } catch (err) {
          setImportError(err instanceof Error ? err.message : String(err));
        }
      };
      reader.onerror = () => setImportError("Failed to read file");
      reader.readAsText(file);
      e.target.value = "";
    },
    [cancelLoopSelection, onChange],
  );

  return (
    <div className="space-y-3">
      <div className="flex flex-wrap items-center gap-2">
        <input
          className={inputCls + " flex-1 text-sm font-semibold"}
          value={script.name}
          onChange={(e) => onChange({ ...script, name: e.target.value })}
          placeholder={t("test.name")}
        />
        <Button variant="primary" onClick={onRun} disabled={executionCount === 0 || runDisabled}>
          <Play size={14} />
          {t("test.run")}
        </Button>
      </div>

      <div className="flex flex-wrap items-center gap-2">
        <Button variant="ghost" onClick={() => fileRef.current?.click()}>
          <FileUp size={14} />
          {t("test.import")}
        </Button>
        <input ref={fileRef} type="file" accept=".ndjson,.jsonl,.json" className="hidden" onChange={handleImport} />
        <Button variant="ghost" onClick={handleExport} disabled={script.steps.length === 0}>
          <Download size={14} />
          {t("test.export")}
        </Button>
        <Button
          variant={selectingLoop ? "default" : "ghost"}
          onClick={() => selectingLoop ? cancelLoopSelection() : setSelectingLoop(true)}
          disabled={!selectingLoop && script.steps.every((item) => isTestLoop(item) && !isTestUnit(item))}
        >
          <SquareCheckBig size={14} />
          {selectingLoop ? t("test.loop.cancelSelection") : t("test.loop.select")}
        </Button>
        <span className="flex-1" />
        <span className="text-[11px] text-ink-dim">
          {t("test.stepCountExpanded", { commands: commandCount, executions: executionCount })}
        </span>
      </div>

      {selectingLoop && (
        <div className="flex flex-wrap items-center gap-2 rounded-xl border border-brand/30 bg-brand/[0.06] px-3 py-2">
          <SquareCheckBig size={14} className="text-brand" />
          <span className="min-w-[180px] flex-1 text-xs text-ink-dim">{t("test.group.selectHint")}</span>
          <Badge tone="brand">{t("test.loop.selected", { n: selectedIndexes.length })}</Badge>
          {selectedIndexes.length > 1 && !selectionIsContiguous && (
            <span className="text-[11px] text-warn">{t("test.loop.nonContiguous")}</span>
          )}
          <input
            className={`${inputCls} min-w-[150px] flex-1`}
            value={unitName}
            maxLength={80}
            onChange={(event) => setUnitName(event.target.value)}
            placeholder={t("test.unit.namePlaceholder")}
            aria-label={t("test.unit.name")}
          />
          <Button variant="default" onClick={createUnit} disabled={!selectionIsContiguous}>
            <Package size={14} />
            {t("test.unit.create")}
          </Button>
          <Button variant="primary" onClick={createLoop} disabled={!selectionIsContiguous}>
            <Repeat2 size={14} />
            {t("test.loop.create")}
          </Button>
        </div>
      )}

      {importError && (
        <div className="rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
          {importError}
        </div>
      )}

      <div className="space-y-1.5">
        {script.steps.map((item, i) => isTestCondition(item) ? (
          <div key={item.id} className="flex min-h-12 items-center gap-2 rounded-xl border border-violet-500/30 bg-violet-500/[0.06] px-3 py-2">
            <GitBranch size={14} className="text-violet-500" />
            <span className="min-w-0 flex-1 text-xs font-semibold text-ink">{t("test.condition.title")}</span>
            <span className="text-[10px] text-ink-dim">{t("test.condition.summary", { then: item.params.then_steps.length, else: item.params.else_steps.length })}</span>
            <button type="button" onClick={() => deleteItem(i)} className="text-ink-dim hover:text-danger" title={t("test.step.delete")}>
              <Trash2 size={13} />
            </button>
          </div>
        ) : isTestLoop(item) ? (
          <LoopCard
            key={item.id}
            loop={item}
            index={i}
            total={script.steps.length}
            onChange={(loop) => updateItem(i, loop)}
            onDelete={() => deleteItem(i)}
            onUngroup={() => ungroupLoop(i, item)}
            onMoveUp={() => moveItem(i, i - 1)}
            onMoveDown={() => moveItem(i, i + 1)}
            maxCount={maxLoopCount(item)}
            selectable={selectingLoop && isTestUnit(item)}
            selected={selectedStepIds.has(item.id)}
            onSelect={() => toggleLoopStep(item.id)}
          />
        ) : (
          <StepCard
            key={item.id}
            step={item}
            index={i}
            total={script.steps.length}
            onChange={(step) => updateItem(i, step)}
            onDelete={() => deleteItem(i)}
            onMoveUp={() => moveItem(i, i - 1)}
            onMoveDown={() => moveItem(i, i + 1)}
            selectable={selectingLoop}
            selected={selectedStepIds.has(item.id)}
            onSelect={() => toggleLoopStep(item.id)}
          />
        ))}
      </div>

      <div className="flex items-center gap-2 pt-1">
        <select
          className={inputCls + " flex-1"}
          value={addType}
          onChange={(e) => setAddType(e.target.value as StepType)}
        >
          {STEP_TYPES.map((type) => (
            <option key={type} value={type}>{t(`test.step.${type}`)}</option>
          ))}
        </select>
        <Button variant="default" onClick={addStep} disabled={executionCount >= MAX_EXECUTION_STEPS}>
          <Plus size={14} />
          {t("test.addStep")}
        </Button>
      </div>

      {script.steps.length === 0 && (
        <div className="rounded-xl border border-dashed border-line/50 py-8 text-center text-xs text-ink-dim">
          {t("test.noSteps")}
        </div>
      )}
    </div>
  );
}
