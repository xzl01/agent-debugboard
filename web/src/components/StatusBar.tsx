import { Languages, RefreshCw } from "lucide-react";
import { Button } from "./ui";
import type { BoardSnapshot, MemoryPressureSnapshot } from "@/lib/types";
import type { OtaStatus } from "@/lib/ota";
import { cn, formatBytes, formatUptime } from "@/lib/utils";
import { useI18n } from "@/lib/i18n";
import { ThemeMenu } from "./ThemeMenu";

const formatPctX100 = (value?: number): string =>
  value == null ? "—" : `${(value / 100).toFixed(2)} %`;

function ramMetricTone(pressurePctX100?: number): string {
  if (pressurePctX100 == null) return "text-ink-dim";
  if (pressurePctX100 >= 9000) return "text-danger";
  if (pressurePctX100 >= 7500) return "text-warn";
  return "text-ink-dim";
}

const formatSince = (value: MemoryPressureSnapshot["since"]): string | null => value ?? null;

export function StatusBar({
  snapshot,
  connected,
  lastVerifiedAt,
  loading,
  auto,
  setAuto,
  live,
  setLive,
  onRefresh,
  ota,
}: {
  snapshot: BoardSnapshot;
  connected: boolean;
  lastVerifiedAt?: number | null;
  loading: boolean;
  auto: boolean;
  setAuto: (v: boolean) => void;
  live: boolean;
  setLive: (v: boolean) => void;
  onRefresh: () => void;
  ota?: OtaStatus | null;
}) {
  const { t, lang, setLang } = useI18n();

  const { temperature: temp, cpu, heap, memory, runtime } = snapshot.monitoring;
  const uptime = runtime.uptime_seconds;

  const tempStr =
    temp.available && temp.celsius
      ? `${(temp.celsius.val1 + temp.celsius.val2 / 1e6).toFixed(1)} °C`
      : "—";
  const cpuStr =
    cpu.available && cpu.active_pct_x100 != null
      ? formatPctX100(cpu.active_pct_x100)
      : "—";
  const heapStr =
    heap.available && heap.free_bytes != null ? formatBytes(heap.free_bytes) : "—";
  const config = snapshot.config;

  const componentLabel = (component?: string): string | null => {
    if (!component) return null;

    const key = `status.memory.component.${component}`;
    const label = t(key);
    return label === key ? component : label;
  };

  const formatLimiter = (pressure?: MemoryPressureSnapshot): string | null => {
    const label = componentLabel(pressure?.limiting_component);
    if (!label) return null;
    return pressure?.limiting_name ? `${label}: ${pressure.limiting_name}` : label;
  };

  const currentPressure: MemoryPressureSnapshot | undefined =
    memory?.current_pressure?.available ? memory.current_pressure : undefined;
  const peakPressure: MemoryPressureSnapshot | undefined =
    memory?.peak_pressure?.available ? memory.peak_pressure : undefined;
  const legacyPressure: MemoryPressureSnapshot | undefined =
    memory?.available && memory.pressure_pct_x100 != null
      ? {
          available: true,
          reason: memory.reason,
          coverage: memory.coverage,
          pressure_pct_x100: memory.pressure_pct_x100,
          limiting_component: memory.limiting_component,
          limiting_name: memory.limiting_name,
        }
      : undefined;

  const primaryPressure: MemoryPressureSnapshot | undefined = currentPressure ?? legacyPressure;
  const primaryPressurePct = primaryPressure?.pressure_pct_x100;
  const primaryPressureStr =
    primaryPressurePct != null ? formatPctX100(primaryPressurePct) : null;
  const primaryLabel = currentPressure ? t("status.memory.currentRam") : t("status.ram");
  const primaryLimiter = formatLimiter(primaryPressure);
  const peakPressureStr =
    peakPressure?.pressure_pct_x100 != null ? formatPctX100(peakPressure.pressure_pct_x100) : null;
  const peakLimiter = formatLimiter(peakPressure);
  const primarySince = formatSince(primaryPressure?.since);
  const peakSince = formatSince(peakPressure?.since);
  const ramTitle = primaryPressureStr
    ? [
        `${primaryLabel}: ${primaryPressureStr}`,
        primaryLimiter ? `${t("status.memory.limiter")}: ${primaryLimiter}` : null,
        primaryPressure?.coverage ? `${t("status.memory.coverage")}: ${primaryPressure.coverage}` : null,
        primaryPressure?.tie_count != null ? `${t("status.memory.ties")}: ${primaryPressure.tie_count}` : null,
        primarySince ? `${t("status.memory.since")}: ${primarySince}` : null,
        primaryPressure?.reason ? `${t("status.memory.reason")}: ${primaryPressure.reason}` : null,
        peakPressureStr ? `${t("status.memory.peak")}: ${peakPressureStr}` : null,
        peakLimiter ? `${t("status.memory.peakLimiter")}: ${peakLimiter}` : null,
        peakPressure?.coverage ? `${t("status.memory.peakCoverage")}: ${peakPressure.coverage}` : null,
        peakPressure?.tie_count != null ? `${t("status.memory.peakTies")}: ${peakPressure.tie_count}` : null,
        peakSince ? `${t("status.memory.peakSince")}: ${peakSince}` : null,
        peakPressure?.reason ? `${t("status.memory.peakReason")}: ${peakPressure.reason}` : null,
      ]
        .filter(Boolean)
        .join(" · ")
    : peakPressureStr
      ? [
          `${t("status.memory.peak")}: ${peakPressureStr}`,
          peakLimiter ? `${t("status.memory.peakLimiter")}: ${peakLimiter}` : null,
        ]
          .filter(Boolean)
          .join(" · ")
      : undefined;
  const heapTitle = peakPressureStr
    ? [
        `${t("status.heap")}: ${heapStr}`,
        `${t("status.memory.peak")}: ${peakPressureStr}`,
        peakLimiter ? `${t("status.memory.peakLimiter")}: ${peakLimiter}` : null,
      ]
        .filter(Boolean)
        .join(" · ")
    : undefined;

  return (
    <header className="sticky top-0 z-30 border-b border-line/80 bg-panel/95">
      <div className="mx-auto flex min-h-14 max-w-[1440px] flex-wrap items-center gap-x-5 px-6 md:flex-nowrap">
        <div className="order-1 flex min-w-0 shrink-0 items-baseline gap-3 md:w-[360px]">
          <h1 className="truncate text-[17px] font-semibold tracking-[-0.02em] text-ink">{t("app.title")}</h1>
          <div className="flex min-w-0 items-center gap-1.5 whitespace-nowrap rounded-lg bg-panel2/70 px-2 py-1 text-[11px] text-ink-dim">
            <span data-testid="status-mcu">{snapshot.mcu?.toUpperCase() || "—"}</span>
            <span aria-hidden="true">·</span>
            <span data-testid="status-usb" className="min-w-20">{snapshot.usb || "—"}</span>
          </div>
        </div>

        <div
          data-testid="status-details"
          className="order-3 flex w-full min-w-0 items-center gap-2 overflow-x-auto whitespace-nowrap py-2 text-[11px] text-ink-dim md:order-2 md:w-auto md:flex-1 md:justify-center md:py-0"
        >
          <span
            data-testid="status-connection"
            className={cn(
              "inline-flex min-h-7 items-center gap-1.5 rounded-lg border border-line/60 bg-panel2/45 px-2 font-medium",
              connected ? "text-ok" : "text-danger",
            )}
          >
            <span aria-hidden="true" className={cn("h-1.5 w-1.5 rounded-full", connected ? "bg-ok" : "bg-danger")} />
            {t(connected ? "status.online" : "status.offline")}
          </span>
          {!connected && lastVerifiedAt != null && (
            <span>
              {t("snapshot.lastVerified", {
                time: new Intl.DateTimeFormat(lang === "zh" ? "zh-CN" : "en", {
                  hour: "2-digit",
                  minute: "2-digit",
                  second: "2-digit",
                  hour12: false,
                }).format(lastVerifiedAt),
              })}
            </span>
          )}
          <div className="inline-flex min-h-7 items-center divide-x divide-line/70 rounded-lg border border-line/60 bg-panel2/35">
            <span className="px-2">{tempStr}</span>
            <span className="px-2">CPU {cpuStr}</span>
            <span
              className={cn("hidden px-2 lg:inline", ramMetricTone(primaryPressurePct))}
              title={ramTitle || heapTitle}
              aria-label={ramTitle || heapTitle}
            >
              RAM {primaryPressureStr || heapStr}
            </span>
            <span className="hidden px-2 xl:inline">{uptime != null ? formatUptime(uptime) : "—"}</span>
          </div>
          <div className="inline-flex items-center gap-0.5 rounded-lg border border-line/60 bg-panel2/55 p-0.5">
            <button
              type="button"
              aria-pressed={auto}
              disabled={live}
              className={cn(
                "min-h-6 rounded-md px-2 transition-colors duration-150 hover:text-ink disabled:cursor-not-allowed disabled:opacity-50",
                auto ? "bg-panel font-medium text-ink ring-1 ring-inset ring-line/60" : "text-ink-dim",
              )}
              onClick={() => setAuto(!auto)}
            >
              {t("status.auto")}
            </button>
            <button
              type="button"
              aria-pressed={live}
              className={cn(
                "min-h-6 rounded-md px-2 transition-colors duration-150 hover:text-ink",
                live ? "bg-panel font-medium text-ink ring-1 ring-inset ring-line/60" : "text-ink-dim",
              )}
              onClick={() => setLive(!live)}
            >
              {t("status.live")}
            </button>
          </div>
          <span
            data-testid="status-persistent-config"
            className={cn(
              "rounded-md px-1.5 py-1",
              config?.pendingCount ? "text-warn" : "text-ink-dim",
            )}
            title={config?.reason || undefined}
          >
            {config?.available
              ? `${t("config.saved")} ${config.savedCount} · ${t("config.pendingCount", { count: config.pendingCount })}`
              : `${t("config.title")} · ${t(config ? "config.status.unavailable" : "config.status.unsupported")}`}
          </span>
          {connected && ota && ota.state !== "idle" && (
            <span className={ota.state === "failed" ? "text-danger" : ota.state === "verified" ? "text-ok" : "text-warn"}>
              {t(`ota.state.${ota.state}`)}
            </span>
          )}
        </div>

        <div className="order-2 ml-auto flex shrink-0 items-center gap-1 border-l border-line/70 pl-2 md:order-3 md:ml-0">
          <Button
            variant="ghost"
            className="min-h-8 rounded-lg px-2 py-1 text-xs"
            onClick={() => setLang(lang === "en" ? "zh" : "en")}
            title={t("lang.toggle")}
            aria-label={t("lang.toggle")}
          >
            <Languages size={14} /> <span className="hidden sm:inline">{t("lang.toggle")}</span>
          </Button>
          <ThemeMenu />
          <Button
            variant="ghost"
            className="h-8 min-h-8 w-8 rounded-lg p-0"
            onClick={onRefresh}
            disabled={loading}
            title={t("status.refresh")}
            aria-label={t("status.refresh")}
          >
            <RefreshCw size={14} className={loading ? "animate-spin" : ""} />
          </Button>
        </div>
      </div>
    </header>
  );
}
