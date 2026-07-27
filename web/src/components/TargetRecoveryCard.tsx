import { useMemo, useState } from "react";
import { Loader2, ShieldAlert, Zap } from "lucide-react";
import type { PowerOutput } from "@/lib/types";
import type { TargetRecoveryMode } from "@/lib/api";
import { useI18n } from "@/lib/i18n";
import { Badge, Button, Card } from "./ui";

const RECOVERY_RAILS = ["5v_out", "12v_out", "20v_out"] as const;

export function TargetRecoveryCard({
  outputs,
  onEnter,
}: {
  outputs: PowerOutput[];
  onEnter: (mode: TargetRecoveryMode, rail: string) => Promise<void>;
}) {
  const { t } = useI18n();
  const [mode, setMode] = useState<TargetRecoveryMode>("rockchip-maskrom");
  const [rail, setRail] = useState("5v_out");
  const [confirming, setConfirming] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [done, setDone] = useState(false);

  const availableRails = useMemo(() => {
    const reported = new Set(outputs.filter((output) => output.controllable).map((output) => output.name));
    const filtered = RECOVERY_RAILS.filter((name) => reported.has(name));
    return filtered.length > 0 ? filtered : RECOVERY_RAILS;
  }, [outputs]);
  const activeLevel = mode === "qualcomm-edl" ? t("targetRecovery.high") : t("targetRecovery.low");

  const run = async () => {
    setBusy(true);
    setError(null);
    setDone(false);
    try {
      await onEnter(mode, rail);
      setDone(true);
      setConfirming(false);
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : String(cause));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Card
      title={t("targetRecovery.title")}
      subtitle={t("targetRecovery.subtitle")}
      icon={Zap}
      right={<Badge tone="warn">CON_MAS</Badge>}
    >
      <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-1">
        <label className="grid gap-1.5 text-xs text-ink-dim">
          {t("targetRecovery.mode")}
          <select
            value={mode}
            onChange={(event) => {
              setMode(event.target.value as TargetRecoveryMode);
              setConfirming(false);
              setDone(false);
            }}
            disabled={busy}
            className="min-h-10 rounded-xl border border-line/70 bg-panel2 px-3 text-sm text-ink outline-none focus-visible:ring-2 focus-visible:ring-brand/40"
          >
            <option value="rockchip-maskrom">Rockchip MASKROM</option>
            <option value="qualcomm-edl">Qualcomm EDL</option>
          </select>
        </label>
        <label className="grid gap-1.5 text-xs text-ink-dim">
          {t("targetRecovery.rail")}
          <select
            value={rail}
            onChange={(event) => {
              setRail(event.target.value);
              setConfirming(false);
              setDone(false);
            }}
            disabled={busy}
            className="min-h-10 rounded-xl border border-line/70 bg-panel2 px-3 text-sm text-ink outline-none focus-visible:ring-2 focus-visible:ring-brand/40"
          >
            {availableRails.map((name) => (
              <option key={name} value={name}>{name.replace("_out", "").toUpperCase()}</option>
            ))}
          </select>
        </label>
      </div>

      <p className="mt-3 text-xs leading-5 text-ink-dim">
        {t("targetRecovery.sequence").replace("{level}", activeLevel)}
      </p>

      {confirming ? (
        <div className="mt-3 rounded-xl border border-warn/40 bg-warn/10 p-3" role="alert">
          <div className="flex gap-2 text-sm text-ink">
            <ShieldAlert size={17} className="mt-0.5 shrink-0 text-warn" />
            <span>
              {t("targetRecovery.confirm")
                .replace("{rail}", rail.replace("_out", "").toUpperCase())
                .replace("{mode}", mode === "qualcomm-edl" ? "Qualcomm EDL" : "Rockchip MASKROM")
                .replace("{level}", activeLevel)}
            </span>
          </div>
          <div className="mt-3 flex flex-wrap gap-2">
            <Button variant="danger" onClick={run} disabled={busy}>
              {busy && <Loader2 size={16} className="animate-spin" />}
              {busy ? t("targetRecovery.running") : t("targetRecovery.confirmAction")}
            </Button>
            <Button variant="default" onClick={() => setConfirming(false)} disabled={busy}>
              {t("targetRecovery.cancel")}
            </Button>
          </div>
        </div>
      ) : (
        <Button
          className="mt-3"
          variant="default"
          onClick={() => {
            setError(null);
            setConfirming(true);
          }}
          disabled={busy}
        >
          <ShieldAlert size={16} />
          {t("targetRecovery.enter")}
        </Button>
      )}

      {done && <p className="mt-3 text-xs text-ok">{t("targetRecovery.done")}</p>}
      {error && <p className="mt-3 text-xs text-danger">{t("targetRecovery.failed")}{error}</p>}
    </Card>
  );
}
