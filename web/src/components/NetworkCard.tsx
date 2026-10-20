import { useEffect, useState } from "react";
import { CheckCircle2, CircleAlert, Loader2, Network } from "lucide-react";
import { getNetSettings, setNetSubnet, type NetSettings } from "@/lib/api";
import { useI18n } from "@/lib/i18n";
import { Button, Card } from "./ui";

function subnetValid(value: number): boolean {
  return Number.isInteger(value) && value >= 1 && value <= 254;
}

function subnetUrl(subnet: number): string {
  return `http://172.29.${subnet}.1/`;
}

type NetworkNotice = {
  readonly subnet: number;
  readonly reconnectRequired: boolean;
  readonly message?: string;
};

export function NetworkCard({
  connected,
  disabled = false,
}: {
  readonly connected: boolean;
  readonly disabled?: boolean;
}) {
  const { t } = useI18n();
  const [settings, setSettings] = useState<NetSettings | null>(null);
  const [input, setInput] = useState("");
  const [loading, setLoading] = useState(false);
  const [applying, setApplying] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<NetworkNotice | null>(null);

  useEffect(() => {
    if (!connected) {
      setLoading(false);
      return;
    }

    let cancelled = false;
    setLoading(true);
    setError(null);
    void getNetSettings()
      .then((next) => {
        if (cancelled) return;
        setSettings(next);
        setInput(String(next.subnet));
      })
      .catch((cause: unknown) => {
        if (cancelled) return;
        setError(cause instanceof Error && cause.message ? cause.message : t("network.error"));
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });

    return () => {
      cancelled = true;
    };
  }, [connected]);

  const parsed = Number(input);
  const inputValid = input.trim() !== "" && subnetValid(parsed);
  const hintSubnet = inputValid ? parsed : settings?.subnet;
  const hintUrl = hintSubnet === undefined ? "—" : subnetUrl(hintSubnet);
  const busy = loading || applying;
  const applyDisabled = !connected || disabled || busy || !inputValid;

  const apply = async () => {
    if (!inputValid) {
      setError(t("network.invalid"));
      setNotice(null);
      return;
    }

    setApplying(true);
    setError(null);
    setNotice(null);
    try {
      const next = await setNetSubnet(parsed);
      const subnetChanged = settings?.subnet !== next.subnet;
      setSettings(next);
      setInput(String(next.subnet));
      setNotice({
        subnet: next.subnet,
        reconnectRequired: next.reconnect_required ?? subnetChanged,
        message: next.message?.trim() || undefined,
      });
    } catch (cause) {
      setError(cause instanceof Error && cause.message ? cause.message : t("network.error"));
    } finally {
      setApplying(false);
    }
  };

  return (
    <Card
      title={t("network.title")}
      subtitle={t("network.subtitle")}
      icon={Network}
    >
      {loading && !settings ? (
        <div className="flex min-h-16 items-center gap-2 text-sm text-ink-dim" role="status">
          <Loader2 size={16} className="animate-spin text-brand" />
          {t("network.loading")}
        </div>
      ) : (
        <div className="space-y-3">
          {settings && (
            <div
              data-testid="network-current"
              className="flex flex-wrap items-center justify-between gap-2 rounded-lg bg-panel2/55 px-3 py-2 text-xs"
            >
              <span className="text-ink-dim">{t("network.current")}</span>
              <span className="font-mono font-semibold text-ink">{settings.subnet}</span>
              <span className="font-mono text-ink-dim">{settings.ip}</span>
            </div>
          )}

          <label className="flex flex-col gap-1.5 text-xs text-ink-dim">
            <span>{t("network.subnet")}</span>
            <input
              data-testid="network-subnet-input"
              type="number"
              min={1}
              max={254}
              step={1}
              value={input}
              disabled={!connected || disabled || busy}
              onChange={(event) => {
                setInput(event.target.value);
                setNotice(null);
              }}
              className="min-h-10 w-full max-w-40 rounded-lg border border-line/80 bg-panel2/80 px-3 font-mono text-sm text-ink outline-none focus-visible:ring-2 focus-visible:ring-brand/40 disabled:cursor-not-allowed disabled:opacity-50"
            />
          </label>

          <p className="text-xs text-ink-dim" data-testid="network-hint">
            {t("network.hint", { url: hintUrl })}
          </p>

          {!connected && <p className="text-xs text-ink-dim">{t("network.disconnected")}</p>}

          {error && (
            <div role="alert" className="flex gap-2 rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
              <CircleAlert size={14} className="mt-0.5 shrink-0" />
              <span className="break-words">{error}</span>
            </div>
          )}

          {notice && (
            <div role="status" className="flex gap-2 rounded-lg border border-ok/30 bg-ok/15 px-3 py-2 text-xs text-ok">
              <CheckCircle2 size={14} className="mt-0.5 shrink-0" />
              <span className="break-words">
                {t("network.success", { url: subnetUrl(notice.subnet) })}
                {notice.message ? <span className="mt-1 block">{notice.message}</span>
                  : notice.reconnectRequired && <span className="mt-1 block">{t("network.reconnectNotice")}</span>}
              </span>
            </div>
          )}

          <Button
            type="button"
            variant="primary"
            data-testid="network-apply"
            disabled={applyDisabled}
            onClick={() => void apply()}
          >
            {applying ? <Loader2 size={15} className="animate-spin" /> : <Network size={15} />}
            {t(applying ? "network.applying" : "network.apply")}
          </Button>
        </div>
      )}
    </Card>
  );
}
