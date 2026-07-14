import { ShieldCheck, ShieldAlert } from "lucide-react";
import { Badge, Card } from "./ui";
import type { WatchdogStatus } from "@/lib/types";
import { useI18n } from "@/lib/i18n";

export function WatchdogCard({ watchdog }: { watchdog: WatchdogStatus }) {
  const { t } = useI18n();
  const healthy = watchdog.healthy;
  return (
    <Card
      title={t("watchdog.title")}
      subtitle={t("watchdog.subtitle")}
      icon={healthy ? ShieldCheck : ShieldAlert}
    >
      <div className="flex items-center gap-2">
        {healthy ? (
          <Badge tone="ok">{t("watchdog.healthy")}</Badge>
        ) : (
          <Badge tone="danger">{t("watchdog.unhealthy")}</Badge>
        )}
        {watchdog.armed && <Badge tone="brand">{t("watchdog.armed")}</Badge>}
        {watchdog.automatic && <Badge tone="neutral">{t("watchdog.automatic")}</Badge>}
        {watchdog.bootloader_on_timeout && (
          <Badge tone="warn">{t("watchdog.bootTimeout")}</Badge>
        )}
      </div>
      <dl className="mt-3 space-y-1.5 text-sm">
        <div className="flex justify-between">
          <dt className="text-ink-dim">{t("watchdog.supported")}</dt>
          <dd className="text-ink">{watchdog.supported ? t("power.on") : t("power.off")}</dd>
        </div>
        <div className="flex justify-between">
          <dt className="text-ink-dim">{t("watchdog.timeout")}</dt>
          <dd className="text-ink">{watchdog.timeout_ms} ms</dd>
        </div>
        <div className="flex justify-between">
          <dt className="text-ink-dim">{t("watchdog.failing")}</dt>
          <dd className="text-ink">{watchdog.failing_service || "—"}</dd>
        </div>
      </dl>
      <p className="mt-3 text-[11px] text-ink-dim">{t("watchdog.note")}</p>
    </Card>
  );
}
