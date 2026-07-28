import {
  RefreshCw,
  EthernetPort,
  Unplug,
  Radio,
  Cpu,
  Thermometer,
  MemoryStick,
  Clock,
  Sun,
  Moon,
  Languages,
  ShieldCheck,
  Upload,
  TriangleAlert,
} from "lucide-react";
import { Badge, Button, Toggle } from "./ui";
import type { BoardSnapshot, MemoryPressureSnapshot } from "@/lib/types";
import type { OtaStatus } from "@/lib/ota";
import { cn, formatBytes, formatUptime } from "@/lib/utils";
import { useI18n } from "@/lib/i18n";
import { useTheme } from "@/lib/theme";

function formatPctX100(value?: number): string {
  if (value == null) return "—";
  return `${(value / 100).toFixed(2)} %`;
}

function ramMetricTone(pressurePctX100?: number): string {
  if (pressurePctX100 == null) return "text-ink-dim";
  if (pressurePctX100 >= 9000) return "text-danger";
  if (pressurePctX100 >= 7500) return "text-warn";
  return "text-ink-dim";
}

function formatSince(value: MemoryPressureSnapshot["since"]): string | null {
  if (value == null) return null;
  return value;
}

export function StatusBar({
  snapshot,
  connected,
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
  loading: boolean;
  auto: boolean;
  setAuto: (v: boolean) => void;
  live: boolean;
  setLive: (v: boolean) => void;
  onRefresh: () => void;
  ota?: OtaStatus | null;
}) {
  const { t, lang, setLang } = useI18n();
  const { theme, toggle: toggleTheme } = useTheme();

  const m = snapshot.monitoring;
  const temp = m.temperature;
  const cpu = m.cpu;
  const heap = m.heap;
  const memory = m.memory;
  const uptime = m.runtime.uptime_seconds;

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
    <header className="sticky top-0 z-20 border-b border-line/70 bg-bg/90 shadow-sm backdrop-blur-md">
      <div className="mx-auto flex max-w-[1600px] items-center justify-between gap-3 px-4 py-3">
        <div className="flex min-w-0 items-center gap-3">
          <div className="grid h-10 w-10 shrink-0 place-items-center rounded-xl bg-brand/15 text-brand">
            <Cpu size={20} />
          </div>
          <div className="min-w-0">
            <h1 className="truncate text-sm font-semibold leading-tight text-ink">
              {t("app.title")}
            </h1>
            <p className="truncate text-xs text-ink-dim">{t("app.subtitle")}</p>
          </div>
        </div>

        <div className="flex shrink-0 items-center gap-1">
          <Button
            variant="ghost"
            className="px-2.5"
            onClick={() => setLang(lang === "en" ? "zh" : "en")}
            title={t("lang.toggle")}
            aria-label={t("lang.toggle")}
          >
            <Languages size={16} /> <span className="hidden sm:inline">{t("lang.toggle")}</span>
          </Button>
          <Button
            variant="ghost"
            className="px-2.5"
            onClick={toggleTheme}
            title={t("theme.toggle")}
            aria-label={`${t("theme.toggle")}: ${theme === "dark" ? t("theme.light") : t("theme.dark")}`}
          >
            {theme === "dark" ? <Sun size={16} /> : <Moon size={16} />}
            <span className="hidden sm:inline">
              {theme === "dark" ? t("theme.light") : t("theme.dark")}
            </span>
          </Button>

          <Button
            variant="ghost"
            className="h-10 w-10 rounded-xl p-0"
            onClick={onRefresh}
            disabled={loading}
            title={t("status.refresh")}
            aria-label={t("status.refresh")}
          >
            <RefreshCw size={16} className={loading ? "animate-spin" : ""} />
          </Button>
        </div>
      </div>

      <div className="mx-auto flex max-w-[1600px] flex-wrap items-center justify-between gap-x-5 gap-y-2 px-4 pb-3">
        <div className="flex min-w-0 flex-wrap items-center gap-x-3 gap-y-1.5">
          {connected ? (
            <Badge tone="ok">
              <EthernetPort size={12} /> {t("status.online")}
            </Badge>
          ) : (
            <Badge tone="danger">
              <Unplug size={12} /> {t("status.offline")}
            </Badge>
          )}
          <Badge tone="neutral">
            <Cpu size={12} /> {snapshot.mcu?.toUpperCase() || "—"}
          </Badge>
          {snapshot.usb && (
            <Badge tone="brand">
              <Radio size={12} /> {snapshot.usb}
            </Badge>
          )}
          {connected && ota && ota.state !== "idle" && (
            <Badge tone={ota.state === "failed" ? "danger" : ota.state === "verified" ? "ok" : "warn"}>
              {ota.state === "uploading" ? <Upload size={12} /> : ota.state === "failed" ? <TriangleAlert size={12} /> : <ShieldCheck size={12} />}
              {" "}{t(`ota.state.${ota.state}`)}
            </Badge>
          )}
          <span className="inline-flex items-center gap-1.5 text-xs text-ink-dim">
            <Thermometer size={13} /> {tempStr}
          </span>
          <span className="inline-flex items-center gap-1.5 text-xs text-ink-dim">
            <Cpu size={13} /> {cpuStr}
          </span>
          {primaryPressureStr ? (
            <span
              className={cn(
                "inline-flex min-w-0 max-w-full items-center gap-1.5 text-xs",
                ramMetricTone(primaryPressurePct)
              )}
              title={ramTitle}
              aria-label={ramTitle}
            >
              <MemoryStick size={13} className="shrink-0" />
              <span className="truncate">{primaryPressureStr}</span>
            </span>
          ) : (
            <span
              className="inline-flex min-w-0 max-w-full items-center gap-1.5 text-xs text-ink-dim"
              title={heapTitle}
              aria-label={heapTitle}
            >
              <MemoryStick size={13} className="shrink-0" />
              <span className="truncate">{heapStr}</span>
            </span>
          )}
          <span className="inline-flex items-center gap-1.5 text-xs text-ink-dim">
            <Clock size={13} /> {uptime != null ? formatUptime(uptime) : "—"}
          </span>
        </div>
        <div className="flex items-center gap-4 rounded-xl border border-line/60 bg-panel/70 px-3 py-1.5">
          <label className="flex items-center gap-2 text-xs text-ink-dim">
            {t("status.auto")}
            <Toggle checked={auto} onChange={setAuto} disabled={live} />
          </label>
          <label className="flex items-center gap-2 text-xs text-ink-dim">
            {t("status.live")}
            <Toggle checked={live} onChange={setLive} />
          </label>
        </div>
      </div>
    </header>
  );
}
