import { ChevronDown, Loader2, ServerCrash, SlidersHorizontal } from "lucide-react";
import { useBoard } from "@/hooks/useBoard";
import { StatusBar } from "./components/StatusBar";
import { PowerCard } from "./components/PowerCard";
import { SwitchCard } from "./components/SwitchCard";
import { GpioCard } from "./components/GpioCard";
import { WatchdogCard } from "./components/WatchdogCard";
import { BootCard } from "./components/BootCard";
import { SerialCard } from "./components/SerialCard";
import { Badge, Button } from "./components/ui";
import { useI18n } from "@/lib/i18n";

export default function App() {
  const board = useBoard();
  const { t } = useI18n();

  return (
    <div className="min-h-full bg-bg text-ink">
      <StatusBar
        snapshot={board.snapshot}
        connected={board.connected}
        loading={board.loading}
        auto={board.auto}
        setAuto={board.setAuto}
        live={board.live}
        setLive={board.setLive}
        onRefresh={board.refresh}
      />

      {!board.connected && (
        <div className="mx-auto max-w-[1400px] px-4 pt-4">
          <div className="flex items-center gap-3 rounded-xl border border-danger/30 bg-danger/10 px-4 py-3">
            <ServerCrash size={20} className="text-danger" />
            <div className="flex-1">
              <div className="text-sm font-medium text-danger">{t("banner.unreachable")}</div>
              <div className="text-xs text-ink-dim">
                {board.error || t("banner.unreachable.detail")}
              </div>
            </div>
            <Button variant="default" onClick={board.refresh}>
              {t("banner.retry")}
            </Button>
          </div>
        </div>
      )}

      <main className="mx-auto max-w-[1400px] px-4 py-5">
        {board.loading && board.connected ? (
          <div className="flex flex-col items-center justify-center gap-3 py-24 text-ink-dim">
            <Loader2 size={24} className="animate-spin text-brand" />
            <span className="text-sm">{t("loading")}</span>
          </div>
        ) : (
          <div className="grid animate-fade-up items-start gap-4 xl:grid-cols-[minmax(340px,400px)_minmax(0,1fr)]">
            <aside className="grid min-w-0 gap-4 sm:grid-cols-2 xl:grid-cols-1">
              <div className="min-w-0 sm:col-span-2 xl:col-span-1">
                <PowerCard
                  outputs={board.snapshot.powerOutputs}
                  readings={board.snapshot.adc}
                  onSet={board.setPower}
                />
              </div>
              <div className="min-w-0">
                <SwitchCard switches={board.snapshot.switches} onSet={board.setSwitch} />
              </div>

              <details className="group min-w-0 rounded-2xl border border-line/70 bg-panel shadow-sm sm:col-span-2 xl:col-span-1">
                <summary className="flex min-h-16 cursor-pointer list-none items-center gap-3 rounded-2xl px-4 py-3 outline-none transition-colors hover:bg-panel2/50 focus-visible:ring-2 focus-visible:ring-brand/40">
                  <span className="grid h-9 w-9 shrink-0 place-items-center rounded-xl bg-brand/10 text-brand">
                    <SlidersHorizontal size={17} />
                  </span>
                  <span className="min-w-0 flex-1">
                    <span className="block text-sm font-semibold text-ink">{t("advanced.title")}</span>
                    <span className="block truncate text-xs text-ink-dim">{t("advanced.subtitle")}</span>
                  </span>
                  <Badge tone="neutral">{t("advanced.count")}</Badge>
                  <ChevronDown
                    size={17}
                    className="shrink-0 text-ink-dim transition-transform duration-200 group-open:rotate-180"
                  />
                </summary>
                <div className="grid min-w-0 gap-4 border-t border-line/60 p-3 lg:grid-cols-2 xl:grid-cols-1">
                  <div className="min-w-0 lg:col-span-2 xl:col-span-1">
                    <GpioCard gpios={board.snapshot.gpios} onSet={board.setGpio} />
                  </div>
                  <WatchdogCard watchdog={board.snapshot.watchdog} />
                  <BootCard onBoot={board.enterBootloader} />
                </div>
              </details>
            </aside>
            <div className="min-w-0 xl:sticky xl:top-[116px]">
              <SerialCard />
            </div>
          </div>
        )}

        <footer className="mt-6 flex flex-wrap items-center justify-between gap-2 text-[11px] text-ink-dim">
          <span>
            {t("footer.endpoint")}{" "}
            <span className="font-mono">http://172.29.203.1:8080/api/v1</span> (proxied by the
            dev server)
          </span>
          <Badge tone="neutral">
            {board.live ? t("footer.live") : t("footer.polling")}
          </Badge>
        </footer>
      </main>
    </div>
  );
}
