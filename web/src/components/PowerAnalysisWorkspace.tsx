import { useState, type RefObject, type ReactNode } from "react";
import { Activity, TimerReset, Zap } from "lucide-react";
import type { SerialAutomationHandle } from "./SerialCard";
import { PowerAnalyzer } from "./PowerAnalyzer";
import { StartupPowerAnalysis } from "./StartupPowerAnalysis";
import { Card } from "./ui";
import type { AutomationTaskControl } from "@/lib/automationTask";
import type { PowerMetric } from "@/lib/power";
import type { CaptureConfig, PowerCapture, PowerOutput, SafeGpio } from "@/lib/types";
import { useI18n } from "@/lib/i18n";

type AnalysisMode = "capture" | "startup";

export function PowerAnalysisWorkspace({
  outputs,
  gpios,
  captureState,
  captureProgress,
  captures,
  serialRef,
  onSetPower,
  onReadPower,
  onArmCapture,
  onTriggerCapture,
  onStopCapture,
  onCancelCapture,
  onClearCaptures,
  taskControl,
  workspaceTabs,
}: {
  outputs: PowerOutput[];
  gpios: SafeGpio[];
  captureState: "idle" | "connecting" | "armed" | "recording" | "receiving";
  captureProgress: {
    received: number;
    total: number;
    persisted?: number;
    queuedChunks?: number;
    dropped?: number;
  } | null;
  captures: PowerCapture[];
  serialRef: RefObject<SerialAutomationHandle>;
  onSetPower: (name: string, on: boolean) => Promise<void>;
  onReadPower: (name: string) => Promise<{ state: string; currentUa: number }>;
  onArmCapture: (config: CaptureConfig) => Promise<void>;
  onTriggerCapture: () => void;
  onStopCapture: () => void;
  onCancelCapture: () => void;
  onClearCaptures: () => void;
  taskControl: AutomationTaskControl;
  workspaceTabs?: ReactNode;
}) {
  const { t } = useI18n();
  const [mode, setMode] = useState<AnalysisMode>("capture");
  const [metric, setMetric] = useState<PowerMetric>("current");

  return (
    <Card
      title={t("powerAnalysis.title")}
      subtitle={t("powerAnalysis.subtitle")}
      icon={Activity}
      headerLeading={workspaceTabs}
    >
      <div className="mb-3 flex flex-wrap items-center justify-between gap-3">
        {mode === "capture" ? (
          <div className="inline-flex min-h-9 items-center gap-2 text-sm font-semibold text-ink">
            <span className="grid h-8 w-8 place-items-center rounded-lg bg-brand/10 text-brand">
              <Activity size={15} />
            </span>
            {t("analyzer.title")}
          </div>
        ) : <span />}
        <div
          role="tablist"
          aria-label={t("powerAnalysis.modes")}
          className="inline-flex rounded-xl border border-line/70 bg-panel2 p-1"
        >
          <button
            type="button"
            role="tab"
            aria-selected={mode === "capture"}
            onClick={() => setMode("capture")}
            className={`inline-flex min-h-8 items-center gap-1.5 rounded-lg px-2.5 text-xs font-medium transition-colors ${
              mode === "capture" ? "bg-panel text-brand shadow-sm" : "text-ink-dim hover:text-ink"
            }`}
          >
            <Activity size={13} />
            {t("powerAnalysis.capture")}
          </button>
          <button
            type="button"
            role="tab"
            aria-selected={mode === "startup"}
            onClick={() => setMode("startup")}
            className={`inline-flex min-h-8 items-center gap-1.5 rounded-lg px-2.5 text-xs font-medium transition-colors ${
              mode === "startup" ? "bg-panel text-brand shadow-sm" : "text-ink-dim hover:text-ink"
            }`}
          >
            <TimerReset size={13} />
            {t("powerAnalysis.startup")}
          </button>
        </div>
      </div>
      {mode === "capture" ? (
        <div>
          <div className="mb-3 flex items-center justify-between gap-3">
            <p className="text-xs text-ink-dim">{t("powerAnalysis.captureHint")}</p>
            <div
              role="tablist"
              aria-label={t("power.chart.metric")}
              className="inline-flex shrink-0 rounded-lg border border-line/70 bg-panel2/60 p-0.5"
            >
              {(["current", "power"] as const).map((value) => (
                <button
                  key={value}
                  type="button"
                  role="tab"
                  aria-selected={metric === value}
                  onClick={() => setMetric(value)}
                  className={`inline-flex min-h-7 items-center gap-1 rounded-md px-2 text-[11px] font-medium transition-colors ${
                    metric === value ? "bg-brand text-on-brand" : "text-ink-dim hover:text-ink"
                  }`}
                >
                  {value === "current" ? <Activity size={12} /> : <Zap size={12} />}
                  {t(`power.chart.${value}`)}
                </button>
              ))}
            </div>
          </div>
          <PowerAnalyzer
            metric={metric}
            gpios={gpios}
            state={captureState}
            progress={captureProgress}
            captures={captures}
            onArm={onArmCapture}
            onTrigger={onTriggerCapture}
            onStop={onStopCapture}
            onCancel={onCancelCapture}
            onClear={onClearCaptures}
            defaultOpen
            showHeader={false}
          />
        </div>
      ) : (
        <StartupPowerAnalysis
          outputs={outputs}
          captureState={captureState}
          captures={captures}
          serialRef={serialRef}
          onSetPower={onSetPower}
          onReadPower={onReadPower}
          onArmCapture={onArmCapture}
          onStopCapture={onStopCapture}
          onCancelCapture={onCancelCapture}
          taskControl={taskControl}
        />
      )}
    </Card>
  );
}
