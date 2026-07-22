import { useCallback, useRef, useState } from "react";
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
  Activity,
  ToggleRight,
  CheckCircle,
  GitBranch,
  type LucideIcon,
} from "lucide-react";
import { Badge, Button } from "./ui";
import type { TestScript, TestStep, StepType, StepAssertion } from "@/lib/testScript";
import {
  STEP_TYPES,
  stepTypeLabel,
  stepTypeIcon,
  defaultStepParams,
  generateStepId,
  parseTestScript,
  serializeTestScript,
} from "@/lib/testScript";
import { USER_POWER_RAILS } from "@/lib/power";
import { downloadBlob } from "@/lib/utils";
import { useI18n } from "@/lib/i18n";

const ICON_MAP: Record<string, LucideIcon> = {
  Zap, Power, Timer, Hourglass, Send, MessageSquareText, Activity,
  ToggleRight, CheckCircle, GitBranch,
};

function iconFor(type: StepType): LucideIcon {
  return ICON_MAP[stepTypeIcon(type)] ?? Zap;
}

function StepParams({
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
            {USER_POWER_RAILS.map((r) => <option key={r} value={r}>{r}</option>)}
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
              <option value="uart0">UART0</option>
              <option value="uart1">UART1</option>
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
              <option value="uart0">UART0</option>
              <option value="uart1">UART1</option>
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
              <option value="uart0">UART0</option>
              <option value="uart1">UART1</option>
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
              <option value="1">1 (HIGH)</option>
              <option value="0">0 (LOW)</option>
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
              <option value="input">input</option>
              <option value="output">output</option>
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
              <option value="sd">SD</option>
              <option value="usb">USB</option>
              <option value="vin">VIN</option>
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

function StepAssertions({
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
  if (step.type === "gpio_assert") {
    availableFields.push({ key: "pin_direction", label: t("test.assert.pinDirection"), default: "output" });
    availableFields.push({ key: "pin_value", label: t("test.assert.pinValue"), default: 1 });
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

function StepCard({
  step,
  index,
  total,
  onChange,
  onDelete,
  onMoveUp,
  onMoveDown,
}: {
  step: TestStep;
  index: number;
  total: number;
  onChange: (s: TestStep) => void;
  onDelete: () => void;
  onMoveUp: () => void;
  onMoveDown: () => void;
}) {
  const [expanded, setExpanded] = useState(false);
  const { t } = useI18n();
  const Icon = iconFor(step.type);

  const updateParam = (key: string, value: unknown) => {
    onChange({ ...step, params: { ...step.params, [key]: value } });
  };

  const updateAssert = (key: string, value: unknown) => {
    const prev = step.assert ?? {};
    onChange({ ...step, assert: { ...prev, [key]: value } });
  };

  const removeAssert = (key: string) => {
    if (!step.assert) return;
    const next = { ...step.assert };
    delete (next as Record<string, unknown>)[key];
    onChange({ ...step, assert: Object.keys(next).length > 0 ? next : undefined });
  };

  const summary = (): string => {
    switch (step.type) {
      case "power_on":
      case "power_off":
        return (step.params as import("@/lib/testScript").PowerOnParams).rail;
      case "delay":
        return `${(step.params as import("@/lib/testScript").DelayParams).ms}ms`;
      case "serial_wait": {
        const p = step.params as import("@/lib/testScript").SerialWaitParams;
        return `${p.channel} "${p.pattern}" ${p.timeout_ms}ms`;
      }
      case "serial_send": {
        const p = step.params as import("@/lib/testScript").SerialSendParams;
        return `${p.channel} "${p.text.replace(/\n/g, "\\n")}"`;
      }
      case "serial_expect": {
        const p = step.params as import("@/lib/testScript").SerialExpectParams;
        return `${p.channel} cmd="${p.command}"`;
      }
      case "adc_read":
        return (step.params as import("@/lib/testScript").AdcReadParams).channel;
      case "gpio_set": {
        const p = step.params as import("@/lib/testScript").GpioSetParams;
        return `${p.pin}=${p.value}`;
      }
      case "gpio_assert": {
        const p = step.params as import("@/lib/testScript").GpioAssertParams;
        return `${p.pin} ${p.direction}`;
      }
      case "switch_route": {
        const p = step.params as import("@/lib/testScript").SwitchRouteParams;
        return `${p.switch} → ${p.route}`;
      }
      case "capture": {
        const p = step.params as import("@/lib/testScript").CaptureParams;
        return `${p.rail} ${p.trigger} ${p.duration_ms}ms`;
      }
    }
  };

  return (
    <div className="rounded-xl border border-line/50 bg-panel2/30 px-3 py-2">
      <div className="flex items-center gap-2">
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
          <span className="ml-2 truncate text-[11px] text-ink-dim">{summary()}</span>
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
          <StepParams step={step} onParamChange={updateParam} />
          <StepAssertions step={step} assert={step.assert} onAssertChange={updateAssert} onAssertRemove={removeAssert} />
        </div>
      )}
    </div>
  );
}

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
}

export function TestEditor({ script, onChange, onRun }: TestEditorProps) {
  const { t } = useI18n();
  const fileRef = useRef<HTMLInputElement>(null);
  const [addType, setAddType] = useState<StepType>("power_on");
  const [importError, setImportError] = useState<string | null>(null);

  const updateStep = useCallback(
    (index: number, step: TestStep) => {
      const steps = [...script.steps];
      steps[index] = step;
      onChange({ ...script, steps });
    },
    [script, onChange],
  );

  const deleteStep = useCallback(
    (index: number) => {
      const steps = script.steps.filter((_, i) => i !== index);
      onChange({ ...script, steps });
    },
    [script, onChange],
  );

  const moveStep = useCallback(
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
          onChange(imported);
        } catch (err) {
          setImportError(err instanceof Error ? err.message : String(err));
        }
      };
      reader.readAsText(file);
      e.target.value = "";
    },
    [onChange],
  );

  return (
    <div className="space-y-3">
      <div className="flex items-center gap-2">
        <input
          className={inputCls + " flex-1 text-sm font-semibold"}
          value={script.name}
          onChange={(e) => onChange({ ...script, name: e.target.value })}
          placeholder={t("test.name")}
        />
        <Button variant="primary" onClick={onRun} disabled={script.steps.length === 0}>
          <Play size={14} />
          {t("test.run")}
        </Button>
      </div>

      <div className="flex items-center gap-2">
        <Button variant="ghost" onClick={() => fileRef.current?.click()}>
          <FileUp size={14} />
          {t("test.import")}
        </Button>
        <input ref={fileRef} type="file" accept=".ndjson,.jsonl,.json" className="hidden" onChange={handleImport} />
        <Button variant="ghost" onClick={handleExport} disabled={script.steps.length === 0}>
          <Download size={14} />
          {t("test.export")}
        </Button>
        <span className="flex-1" />
        <span className="text-[11px] text-ink-dim">{script.steps.length} steps</span>
      </div>

      {importError && (
        <div className="rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
          {importError}
        </div>
      )}

      <div className="space-y-1.5">
        {script.steps.map((step, i) => (
          <StepCard
            key={step.id}
            step={step}
            index={i}
            total={script.steps.length}
            onChange={(s) => updateStep(i, s)}
            onDelete={() => deleteStep(i)}
            onMoveUp={() => moveStep(i, i - 1)}
            onMoveDown={() => moveStep(i, i + 1)}
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
        <Button variant="default" onClick={addStep}>
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
