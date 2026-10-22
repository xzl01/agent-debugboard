import { useEffect, useState } from "react";
import { CheckCircle2, CircleAlert, Loader2, Network } from "lucide-react";
import {
  getNetSettings,
  isLocallyAdministeredMac,
  isPrivateSlash30,
  setNetIdentity,
  type NetSettings,
} from "@/lib/api";
import { useI18n } from "@/lib/i18n";
import { Button, Card } from "./ui";

const networkBase = (value: string) => value.trim().replace(/\/30$/u, "");

export function NetworkCard({
  connected,
  disabled = false,
}: {
  readonly connected: boolean;
  readonly disabled?: boolean;
}) {
  const { t } = useI18n();
  const [settings, setSettings] = useState<NetSettings | null>(null);
  const [macInput, setMacInput] = useState("");
  const [networkInput, setNetworkInput] = useState("");
  const [loading, setLoading] = useState(false);
  const [applying, setApplying] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<{ url: string; reconnectRequired: boolean; message?: string } | null>(null);

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
        setMacInput(next.mac);
        setNetworkInput(`${next.network}/30`);
      })
      .catch((cause: unknown) => {
        if (!cancelled) setError(cause instanceof Error && cause.message ? cause.message : t("network.error"));
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => {
      cancelled = true;
    };
  }, [connected]);

  const macValid = isLocallyAdministeredMac(macInput);
  const networkValid = isPrivateSlash30(networkBase(networkInput));
  const macChanged = settings !== null && macInput.trim().toLowerCase() !== settings.mac;
  const networkChanged = settings !== null && networkBase(networkInput) !== settings.network;
  const changed = macChanged || networkChanged;
  const busy = loading || applying;
  const applyDisabled = !connected || disabled || busy || !macValid || !networkValid || !changed;

  const apply = async () => {
    if (!macValid || !networkValid) {
      setError(t(!macValid ? "network.invalidMac" : "network.invalidNetwork"));
      setNotice(null);
      return;
    }
    setApplying(true);
    setError(null);
    setNotice(null);
    try {
      const next = await setNetIdentity({
        ...(macChanged ? { mac: macInput.trim().toLowerCase() } : {}),
        ...(networkChanged ? { network: networkBase(networkInput) } : {}),
      });
      setSettings(next);
      setMacInput(next.mac);
      setNetworkInput(`${next.network}/30`);
      setNotice({
        url: next.url,
        reconnectRequired: next.reconnect_required ?? (next.mac !== settings?.mac || next.network !== settings?.network),
        message: next.message?.trim() || undefined,
      });
    } catch (cause) {
      setError(cause instanceof Error && cause.message ? cause.message : t("network.error"));
    } finally {
      setApplying(false);
    }
  };

  return (
    <Card title={t("network.title")} subtitle={t("network.subtitle")} icon={Network}>
      {loading && !settings ? (
        <div className="flex min-h-16 items-center gap-2 text-sm text-ink-dim" role="status">
          <Loader2 size={16} className="animate-spin text-brand" />
          {t("network.loading")}
        </div>
      ) : (
        <div className="space-y-3">
          {settings && (
            <div data-testid="network-current" className="flex flex-wrap items-center gap-x-3 gap-y-1 rounded-lg bg-panel2/55 px-3 py-2 font-mono text-xs text-ink">
              <span className="font-semibold">{settings.mac}</span>
              <span>{settings.network}/30</span>
              <span className="text-ink-dim">{settings.ip}</span>
              <span className="rounded-full border border-line/70 px-2 py-0.5 font-sans text-[11px] text-ink-dim">
                {t(settings.origin === "manual" ? "network.origin.manual" : "network.origin.derived")}
              </span>
            </div>
          )}
          <div className="grid gap-3 sm:grid-cols-2">
            <label className="flex flex-col gap-1.5 text-xs text-ink-dim">
              <span>{t("network.mac")}</span>
              <input
                data-testid="network-mac-input"
                type="text"
                spellCheck={false}
                autoCapitalize="none"
                autoComplete="off"
                value={macInput}
                disabled={!connected || disabled || busy}
                onChange={(event) => {
                  setMacInput(event.target.value);
                  setNotice(null);
                }}
                className="min-h-10 w-full rounded-lg border border-line/80 bg-panel2/80 px-3 font-mono text-sm text-ink outline-none focus-visible:ring-2 focus-visible:ring-brand/40 disabled:cursor-not-allowed disabled:opacity-50"
              />
            </label>
            <label className="flex flex-col gap-1.5 text-xs text-ink-dim">
              <span>{t("network.network")}</span>
              <input
                data-testid="network-input"
                type="text"
                spellCheck={false}
                autoCapitalize="none"
                autoComplete="off"
                value={networkInput}
                disabled={!connected || disabled || busy}
                onChange={(event) => {
                  setNetworkInput(event.target.value);
                  setNotice(null);
                }}
                className="min-h-10 w-full rounded-lg border border-line/80 bg-panel2/80 px-3 font-mono text-sm text-ink outline-none focus-visible:ring-2 focus-visible:ring-brand/40 disabled:cursor-not-allowed disabled:opacity-50"
              />
            </label>
          </div>
          <p className="text-xs text-ink-dim" data-testid="network-hint">
            {t("network.hint", { url: settings?.url ?? "—" })}
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
                {t("network.success", { url: notice.url })}
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
