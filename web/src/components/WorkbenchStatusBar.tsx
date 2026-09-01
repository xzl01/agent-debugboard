import { Activity, GitBranch, SquareTerminal, Zap } from "lucide-react";
import { useI18n } from "@/lib/i18n";
import { switchRouteLabel } from "@/lib/switches";
import type { AutomationTaskOwner } from "@/lib/automationTask";
import type { BoardSnapshot, PowerOutput } from "@/lib/types";
import { powerOutputIsOn } from "@/lib/power";
import { cn } from "@/lib/utils";
import type { SerialConnectionSummary } from "./SerialCard";

const RAILS = [
  { name: "5v_out", label: "5V" },
  { name: "12v_out", label: "12V" },
  { name: "20v_out", label: "20V" },
  { name: "vdd_5v", label: "VDD_5V" },
] as const;

type RailState = "on" | "off" | "unknown";

function railState(output?: PowerOutput): RailState {
  if (!output) return "unknown";
  if (powerOutputIsOn(output)) return "on";
  if (output.state === "off" || output.value === 0) return "off";
  return "unknown";
}

function taskLabel(owner: AutomationTaskOwner | null, t: ReturnType<typeof useI18n>["t"]): string {
  return owner ? t(`statusBar.task.${owner}`) : t("statusBar.task.idle");
}

export function WorkbenchStatusBar({
  snapshot,
  connected,
  serialConnections,
  taskOwner,
}: {
  readonly snapshot: BoardSnapshot;
  readonly connected: boolean;
  readonly serialConnections: SerialConnectionSummary;
  readonly taskOwner: AutomationTaskOwner | null;
}) {
  const { t } = useI18n();
  const outputs = new Map(snapshot.powerOutputs.map((output) => [output.name, output]));
  const route = (name: string) => {
    const value = snapshot.switches[name]?.route;
    return value ? switchRouteLabel(t, value) : "—";
  };
  const uartLabel = serialConnections.uart0 && serialConnections.uart1
    ? t("statusBar.uart.bothConnected")
    : serialConnections.uart0
      ? t("statusBar.uart.connected", { channel: "UART0" })
      : serialConnections.uart1
        ? t("statusBar.uart.connected", { channel: "UART1" })
        : t("statusBar.uart.disconnected", { channel: "UART0" });

  return (
    <aside
      aria-label={t("statusBar.label")}
      data-testid="workbench-status-bar"
      className="fixed inset-x-0 bottom-0 z-40 flex h-5 items-center justify-between gap-3 overflow-hidden border-t border-line/90 bg-panel/95 px-3 text-[9px] text-ink-dim sm:h-6 sm:px-6 sm:text-[10px]"
    >
      <div className="flex shrink-0 items-center gap-1.5 whitespace-nowrap font-medium text-ink">
        <span
          aria-hidden="true"
          className={cn("h-1.5 w-1.5 rounded-full", connected ? "bg-ok" : "bg-danger")}
        />
        <span>{t(connected ? "statusBar.boardOnline" : "statusBar.boardOffline")}</span>
      </div>

      <div className="ml-auto flex min-w-0 items-center gap-2.5 overflow-hidden whitespace-nowrap sm:gap-3.5">
        <div className="flex shrink-0 items-center gap-1.5" title={t("statusBar.powerTitle")}>
          <Zap aria-hidden="true" className="h-2.5 w-2.5 sm:h-3 sm:w-3" />
          <div className="flex items-center gap-1.5 sm:gap-2.5">
            {RAILS.map((rail) => {
              const state = railState(outputs.get(rail.name));
              const stateLabel = t(`statusBar.rail.${state}`);
              return (
                <span
                  key={rail.name}
                  data-testid={`status-rail-${rail.name}`}
                  className="flex items-center gap-[3px]"
                  title={`${rail.label} ${stateLabel}`}
                >
                  <span
                    aria-hidden="true"
                    data-testid={`status-rail-lamp-${rail.name}`}
                    className={cn(
                      "h-[5px] w-[5px] rounded-full sm:h-1.5 sm:w-1.5",
                      state === "on"
                        ? "bg-ok ring-2 ring-ok/15"
                        : state === "off"
                          ? "bg-line ring-1 ring-inset ring-ink-dim/20"
                          : "bg-transparent ring-1 ring-inset ring-ink-dim/45",
                    )}
                  />
                  <span>{rail.label} {stateLabel}</span>
                </span>
              );
            })}
          </div>
        </div>

        <div
          className="flex min-w-0 shrink-0 items-center gap-1 border-l border-line/70 pl-2.5 sm:pl-3.5"
          title={t("statusBar.routingTitle")}
        >
          <GitBranch aria-hidden="true" className="h-2.5 w-2.5 shrink-0 sm:h-3 sm:w-3" />
          <span>SD→{route("sd")}</span>
          <span className="hidden sm:inline">· USB→{route("usb")}</span>
          <span className="hidden sm:inline">· TF {route("tf_wp")}</span>
          <span className="hidden sm:inline">· VIN {route("vin")}</span>
        </div>

        <div className="hidden shrink-0 items-center gap-1 border-l border-line/70 pl-3.5 lg:flex" title={t("statusBar.uartTitle")}>
          <SquareTerminal aria-hidden="true" className="h-3 w-3" />
          <span>{uartLabel}</span>
        </div>

        <div
          className={cn("hidden shrink-0 items-center gap-1 border-l border-line/70 pl-3.5 lg:flex", taskOwner && "text-brand")}
          title={t("statusBar.taskTitle")}
        >
          <Activity aria-hidden="true" className="h-3 w-3" />
          <span>{taskLabel(taskOwner, t)}</span>
        </div>
      </div>
    </aside>
  );
}
