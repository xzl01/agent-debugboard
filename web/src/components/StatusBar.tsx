import {
  RefreshCw,
  Wifi,
  WifiOff,
  Radio,
  Cpu,
  Thermometer,
  MemoryStick,
  Clock,
  Sun,
  Moon,
  Languages,
} from "lucide-react";
import { Badge, Button, Toggle } from "./ui";
import type { BoardSnapshot } from "@/lib/types";
import { formatBytes, formatUptime } from "@/lib/utils";
import { useI18n } from "@/lib/i18n";
import { useTheme } from "@/lib/theme";

export function StatusBar({
  snapshot,
  connected,
  loading,
  auto,
  setAuto,
  live,
  setLive,
  onRefresh,
}: {
  snapshot: BoardSnapshot;
  connected: boolean;
  loading: boolean;
  auto: boolean;
  setAuto: (v: boolean) => void;
  live: boolean;
  setLive: (v: boolean) => void;
  onRefresh: () => void;
}) {
  const { t, lang, setLang } = useI18n();
  const { theme, toggle: toggleTheme } = useTheme();

  const m = snapshot.monitoring;
  const temp = m.temperature;
  const cpu = m.cpu;
  const heap = m.heap;
  const uptime = m.runtime.uptime_seconds;

  const tempStr =
    temp.available && temp.celsius
      ? `${(temp.celsius.val1 + temp.celsius.val2 / 1e6).toFixed(1)} °C`
      : "—";
  const cpuStr =
    cpu.available && cpu.active_pct_x100 != null
      ? `${(cpu.active_pct_x100 / 100).toFixed(0)} %`
      : "—";
  const heapStr =
    heap.available && heap.free_bytes != null ? formatBytes(heap.free_bytes) : "—";

  return (
    <header className="sticky top-0 z-20 border-b border-line/70 bg-bg/90 shadow-sm backdrop-blur-md">
      <div className="mx-auto flex max-w-[1400px] items-center justify-between gap-3 px-4 py-3">
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

      <div className="mx-auto flex max-w-[1400px] flex-wrap items-center justify-between gap-x-5 gap-y-2 px-4 pb-3">
        <div className="flex min-w-0 flex-wrap items-center gap-x-3 gap-y-1.5">
          {connected ? (
            <Badge tone="ok">
              <Wifi size={12} /> {t("status.online")}
            </Badge>
          ) : (
            <Badge tone="danger">
              <WifiOff size={12} /> {t("status.offline")}
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
          <span className="inline-flex items-center gap-1.5 text-xs text-ink-dim">
            <Thermometer size={13} /> {tempStr}
          </span>
          <span className="inline-flex items-center gap-1.5 text-xs text-ink-dim">
            <Cpu size={13} /> {cpuStr}
          </span>
          <span className="inline-flex items-center gap-1.5 text-xs text-ink-dim">
            <MemoryStick size={13} /> {heapStr} {t("status.heap")}
          </span>
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
