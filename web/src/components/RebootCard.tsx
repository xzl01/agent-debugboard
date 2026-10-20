import { useRef, useState } from "react";
import { Loader2, RotateCw } from "lucide-react";
import type { AutomationTaskControl } from "@/lib/automationTask";
import { useI18n } from "@/lib/i18n";
import { Badge, Button, Card } from "./ui";

export function RebootCard({ connected, restarting, restartTimedOut, disabled, taskControl, onReboot }: {
  connected: boolean;
  restarting: boolean;
  restartTimedOut: boolean;
  disabled: boolean;
  taskControl: AutomationTaskControl;
  /** The app owns the reboot task lock so it survives maintenance navigation. */
  onReboot: () => Promise<void>;
}) {
  const { t } = useI18n();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const pendingRef = useRef(false);
  const blocked = disabled || !connected || restarting || busy || taskControl.owner !== null;
  const trigger = async () => {
    if (blocked || pendingRef.current) return;
    if (!window.confirm(t("reboot.confirm"))) return;
    pendingRef.current = true;
    setBusy(true);
    setError(null);
    try {
      await onReboot();
    } catch (reason) {
      setError(reason instanceof Error && "code" in reason && reason.code === "reboot_request_timeout"
        ? t("reboot.requestTimeout")
        : `${t("reboot.failed")}${reason instanceof Error ? reason.message : String(reason)}`);
    } finally {
      pendingRef.current = false;
      setBusy(false);
    }
  };

  return (
    <Card title={t("reboot.title")} subtitle={t("reboot.subtitle")} icon={RotateCw} right={<Badge tone="neutral">{t("reboot.normalBoot")}</Badge>}>
      <p className="text-sm leading-6 text-ink-dim">{t("reboot.description")}</p>
      <p className="mt-2 rounded-xl border border-line/70 bg-panel2/50 px-3 py-2.5 text-xs leading-5 text-ink-dim">{t("reboot.notBootsel")}</p>
      <Button type="button" disabled={blocked} onClick={() => void trigger()} className="mt-4">
        {restarting || busy ? <Loader2 size={15} className="animate-spin" aria-hidden="true" /> : <RotateCw size={15} aria-hidden="true" />}
        {t(restarting || busy ? "reboot.restarting" : "reboot.action")}
      </Button>
      {(restarting || busy) && <p role="status" className="mt-3 text-xs leading-5 text-brand">{t("reboot.wait")}</p>}
      {!restarting && restartTimedOut && <p role="status" className="mt-3 text-xs leading-5 text-warn">{t("reboot.reconnectTimeout")}</p>}
      {!connected && !restarting && <p className="mt-3 text-xs text-ink-dim">{t("reboot.disconnected")}</p>}
      {error && <p role="alert" className="mt-3 text-xs text-danger">{error}</p>}
    </Card>
  );
}
