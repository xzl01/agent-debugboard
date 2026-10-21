import { Activity, GitBranch, SquareTerminal, Zap } from "lucide-react";
import { useI18n } from "@/lib/i18n";
import { switchNameLabel, switchRouteLabel } from "@/lib/switches";
import type { AutomationTaskOwner } from "@/lib/automationTask";
import type { BoardSnapshot, PowerOutput } from "@/lib/types";
import { powerOutputIsOn, powerRailLabel } from "@/lib/power";
import { cn } from "@/lib/utils";
import type { SerialConnectionSummary } from "./SerialCard";

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
  const routes = Object.entries(snapshot.switches);
  const serialLabel = t(`statusBar.j4.${serialConnections.state}`);

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
            {snapshot.powerOutputs.map((rail) => {
              const state = railState(rail);
              const label = powerRailLabel(rail.name).toUpperCase();
              const stateLabel = t(`statusBar.rail.${state}`);
              return (
                <span
                  key={rail.name}
                  data-testid={`status-rail-${rail.name}`}
                  className="flex items-center gap-[3px]"
                  title={`${label} ${stateLabel}`}
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
                  <span>{label} {stateLabel}</span>
                </span>
              );
            })}
          </div>
        </div>

        {routes.length > 0 && <div
          className="flex min-w-0 shrink-0 items-center gap-1 border-l border-line/70 pl-2.5 sm:pl-3.5"
          title={t("statusBar.routingTitle")}
        >
          <GitBranch aria-hidden="true" className="h-2.5 w-2.5 shrink-0 sm:h-3 sm:w-3" />
          {routes.map(([name, descriptor], index) => {
            const value = descriptor.route ? switchRouteLabel(t, descriptor.route) : "—";
            return <span key={name} data-testid={`status-route-${name}`}
              className={index > 0 ? "hidden sm:inline-flex" : "inline-flex"}
              title={`${switchNameLabel(t, name)} → ${value}`}>
              {index > 0 && <span aria-hidden="true" className="mr-1">·</span>}
              {name.toUpperCase()}→{value}
            </span>;
          })}
        </div>}

        <div className="hidden shrink-0 items-center gap-1 border-l border-line/70 pl-3.5 lg:flex" title={t("statusBar.j4Title")}>
          <SquareTerminal aria-hidden="true" className="h-3 w-3" />
          <span>{serialLabel}</span>
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
