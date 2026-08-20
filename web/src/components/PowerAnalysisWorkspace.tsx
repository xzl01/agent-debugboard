import { useState, type RefObject, type ReactNode } from "react";
import { Activity, TimerReset } from "lucide-react";
import type { SerialAutomationHandle } from "./SerialCard";
import { PowerAnalyzer } from "./PowerAnalyzer";
import { StartupPowerAnalysis } from "./StartupPowerAnalysis";
import { Card, WorkspaceModeHeader, WorkspaceModeTab } from "./ui";
import type { AutomationTaskControl } from "@/lib/automationTask";
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
  const modeSwitchLocked = captureState !== "idle" || taskControl.owner === "startup" || taskControl.owner === "power";

  return (
    <Card headerLeading={workspaceTabs} contentClassName="p-0">
      <WorkspaceModeHeader
        icon={Activity}
        title={t("powerAnalysis.title")}
        subtitle={t("powerAnalysis.subtitle")}
      >
        <div
          role="tablist"
          aria-label={t("powerAnalysis.modes")}
          data-testid="power-analysis-mode-switch"
          className="grid min-w-0 grid-cols-2 gap-1 rounded-xl border border-line/70 bg-panel2/70 p-1"
        >
          <WorkspaceModeTab
            role="tab"
            id="power-analysis-mode-capture"
            aria-controls="power-analysis-panel-capture"
            aria-selected={mode === "capture"}
            disabled={modeSwitchLocked && mode !== "capture"}
            onClick={() => setMode("capture")}
            selected={mode === "capture"}
            icon={Activity}
            label={t("powerAnalysis.capture")}
            summary={t("powerAnalysis.captureSummary")}
          />
          <WorkspaceModeTab
            role="tab"
            id="power-analysis-mode-startup"
            aria-controls="power-analysis-panel-startup"
            aria-selected={mode === "startup"}
            disabled={modeSwitchLocked && mode !== "startup"}
            onClick={() => setMode("startup")}
            selected={mode === "startup"}
            icon={TimerReset}
            label={t("powerAnalysis.startup")}
            summary={t("powerAnalysis.startupSummary")}
          />
        </div>
      </WorkspaceModeHeader>
      {mode === "capture" ? (
        <div
          id="power-analysis-panel-capture"
          role="tabpanel"
          aria-labelledby="power-analysis-mode-capture"
          className="p-3 sm:p-4"
        >
          <PowerAnalyzer
            gpios={gpios}
            state={captureState}
            progress={captureProgress}
            captures={captures}
            onArm={onArmCapture}
            onTrigger={onTriggerCapture}
            onStop={onStopCapture}
            onCancel={onCancelCapture}
            onClear={onClearCaptures}
            taskControl={taskControl}
            defaultOpen
            showHeader={false}
          />
        </div>
      ) : (
        <div
          id="power-analysis-panel-startup"
          role="tabpanel"
          aria-labelledby="power-analysis-mode-startup"
          className="p-3 sm:p-4"
        >
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
        </div>
      )}
    </Card>
  );
}
