import { useState } from "react";
import { Usb, RotateCcw, ShieldAlert } from "lucide-react";
import { Badge, Button, Card } from "./ui";
import { useI18n } from "@/lib/i18n";
import type { AutomationTaskControl } from "@/lib/automationTask";

export function BootCard({
  onBoot,
  className,
  disabled = false,
  taskControl,
}: {
  onBoot: () => Promise<void>;
  className?: string;
  disabled?: boolean;
  taskControl?: AutomationTaskControl;
}) {
  const { t } = useI18n();
  const [busy, setBusy] = useState(false);
  const [done, setDone] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const trigger = async () => {
    if (disabled) return;
    if (!window.confirm(t("boot.confirm"))) {
      return;
    }
    setDone(false);
    setError(null);
    if (taskControl && !taskControl.acquire("boot")) {
      setError(t("task.error.busy"));
      return;
    }
    setBusy(true);
    try {
      await onBoot();
      setDone(true);
    } catch (e) {
      setError(`${t("boot.failed")}${e instanceof Error ? e.message : String(e)}`);
    } finally {
      setBusy(false);
      taskControl?.release("boot");
    }
  };

  return (
    <Card
      title={t("boot.title")}
      subtitle={t("boot.subtitle")}
      icon={Usb}
      right={<Badge tone="danger"><ShieldAlert size={12} /> {t("boot.danger")}</Badge>}
      className={`border-danger/35 ${className ?? ""}`}
    >
      <p className="text-sm leading-6 text-ink-dim">
        {t("boot.desc")}
        <span className="font-mono">bootloader</span>
        {t("boot.desc2")}
      </p>
      <div className="mt-3 rounded-xl border border-danger/30 bg-danger/5 px-3 py-3 text-xs leading-5 text-ink-dim">
        <p className="font-medium text-danger">{t("boot.flash.complete")}</p>
        <p className="mt-1">{t("boot.flash.neverZephyr")}</p>
        <p className="mt-1">{t("boot.flash.ota")}</p>
      </div>
      <div className="mt-3">
        <Button variant="danger" onClick={trigger} disabled={disabled || busy}>
          <RotateCcw size={16} />
          {busy ? t("boot.rebooting") : t("boot.enter")}
        </Button>
      </div>
      {done && (
        <p className="mt-3 text-[11px] text-ok">{t("boot.done")}</p>
      )}
      {error && <p role="alert" className="mt-3 text-xs text-danger">{error}</p>}
    </Card>
  );
}
