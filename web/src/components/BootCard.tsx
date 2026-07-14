import { useState } from "react";
import { Usb, RotateCcw } from "lucide-react";
import { Button, Card } from "./ui";
import { useI18n } from "@/lib/i18n";

export function BootCard({
  onBoot,
  className,
}: {
  onBoot: () => Promise<void>;
  className?: string;
}) {
  const { t } = useI18n();
  const [busy, setBusy] = useState(false);
  const [done, setDone] = useState(false);

  const trigger = async () => {
    if (!window.confirm(t("boot.confirm"))) {
      return;
    }
    setBusy(true);
    try {
      await onBoot();
      setDone(true);
    } catch (e) {
      window.alert(`${t("boot.failed")}${e instanceof Error ? e.message : String(e)}`);
    } finally {
      setBusy(false);
    }
  };

  return (
    <Card
      title={t("boot.title")}
      subtitle={t("boot.subtitle")}
      icon={Usb}
      className={className}
    >
      <p className="text-sm text-ink-dim">
        {t("boot.desc")}
        <span className="font-mono">bootloader</span>
        {t("boot.desc2")}
      </p>
      <div className="mt-3">
        <Button variant="danger" onClick={trigger} disabled={busy}>
          <RotateCcw size={16} />
          {busy ? t("boot.rebooting") : t("boot.enter")}
        </Button>
      </div>
      {done && (
        <p className="mt-3 text-[11px] text-ok">{t("boot.done")}</p>
      )}
    </Card>
  );
}
