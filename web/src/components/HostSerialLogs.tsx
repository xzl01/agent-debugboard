import { useCallback, useEffect, useState } from "react";
import { Archive, Download, Pin, PinOff, RefreshCw, Trash2 } from "lucide-react";
import { Badge, Button } from "./ui";
import { useI18n } from "@/lib/i18n";
import {
  deleteHostSerialLog,
  getHostSerialLogStatus,
  hostSerialLogDownloadUrl,
  listHostSerialLogs,
  setHostSerialLogPinned,
  type HostSerialLog,
  type HostSerialLogStatus,
} from "@/lib/hostSerialLogs";

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(1)} GiB`;
}

export function HostSerialLogs() {
  const { t } = useI18n();
  const [status, setStatus] = useState<HostSerialLogStatus | null>(null);
  const [logs, setLogs] = useState<HostSerialLog[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [workingId, setWorkingId] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const [nextStatus, nextLogs] = await Promise.all([
        getHostSerialLogStatus(),
        listHostSerialLogs(),
      ]);
      setStatus(nextStatus);
      setLogs(nextLogs);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  async function togglePinned(log: HostSerialLog) {
    setWorkingId(log.session_id);
    setError(null);
    try {
      await setHostSerialLogPinned(log.session_id, !log.pinned);
      await refresh();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setWorkingId(null);
    }
  }

  async function remove(log: HostSerialLog) {
    if (!window.confirm(t("serial.hostLogs.deleteConfirm"))) return;
    setWorkingId(log.session_id);
    setError(null);
    try {
      await deleteHostSerialLog(log.session_id);
      await refresh();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setWorkingId(null);
    }
  }

  const tone = !status?.enabled
    ? "neutral"
    : status.state === "degraded"
      ? "warn"
      : status.active_sessions > 0
        ? "ok"
        : "brand";
  const quotaPercent = status?.quota_bytes
    ? Math.min(100, Math.max(0, (status.total_bytes / status.quota_bytes) * 100))
    : 0;

  return (
    <section id="serial-logs" data-testid="host-serial-logs" className="space-y-4">
      <div className="rounded-xl border border-line/70 bg-panel2/35 p-3">
        <div className="flex items-start gap-3">
          <span className="grid h-8 w-8 shrink-0 place-items-center rounded-lg bg-brand/10 text-brand">
            <Archive size={15} aria-hidden="true" />
          </span>
          <div className="min-w-0 flex-1">
            <div className="flex flex-wrap items-center justify-between gap-2">
              <h4 className="text-xs font-semibold text-ink">{t("serial.hostLogs.title")}</h4>
              <Badge tone={tone}>
                {status?.active_sessions
                  ? t("serial.hostLogs.recording")
                  : status?.enabled
                    ? t("serial.hostLogs.ready")
                    : t("serial.hostLogs.off")}
              </Badge>
            </div>
            <p className="mt-1 text-[11px] text-ink-dim">
              {status?.enabled
                ? t("serial.hostLogs.summary")
                    .replaceAll("{used}", formatBytes(status.total_bytes))
                    .replaceAll("{quota}", formatBytes(status.quota_bytes))
                : t("serial.hostLogs.unavailable")}
            </p>
            <div className="mt-2 h-1.5 overflow-hidden rounded-full bg-line/50" aria-hidden="true">
              <div
                className={`h-full rounded-full ${status?.state === "degraded" ? "bg-warn" : "bg-brand"}`}
                style={{ width: `${quotaPercent}%` }}
              />
            </div>
          </div>
        </div>
        <div className="mt-3 flex items-start justify-between gap-3 border-t border-line/50 pt-3 text-[11px] leading-4 text-ink-dim">
          <span>{t("serial.hostLogs.bridgeOnly")}</span>
          <Button
            type="button"
            variant="ghost"
            className="h-8 min-h-8 shrink-0 px-2 py-1 text-[11px]"
            disabled={loading}
            onClick={() => void refresh()}
          >
            <RefreshCw size={14} className={loading ? "animate-spin" : ""} />
            {t("serial.hostLogs.refresh")}
          </Button>
        </div>
      </div>

      <div className="space-y-3">
        {error && (
          <div role="alert" className="rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
            {t("serial.hostLogs.error").replaceAll("{error}", error)}
          </div>
        )}

        {status?.dropped_bytes ? (
          <div className="rounded-lg border border-warn/30 bg-warn/10 px-3 py-2 text-xs text-warn">
            {t("serial.hostLogs.dropped").replaceAll("{bytes}", formatBytes(status.dropped_bytes))}
          </div>
        ) : null}

        {loading && logs.length === 0 ? (
          <div className="h-16 animate-pulse rounded-lg bg-line/30" aria-label={t("serial.hostLogs.loading")} />
        ) : logs.length === 0 ? (
          <div className="rounded-lg border border-dashed border-line/70 px-3 py-5 text-center text-xs text-ink-dim">
            {status?.enabled ? t("serial.hostLogs.empty") : t("serial.hostLogs.startHost")}
          </div>
        ) : (
          <ul className="space-y-2">
            {logs.map((log) => (
              <li
                key={log.session_id}
                data-testid={`host-serial-log-${log.session_id}`}
                data-integrity={log.complete ? "complete" : "incomplete"}
                className="rounded-xl border border-line/70 bg-panel px-3 py-3"
              >
                <div className="flex items-start justify-between gap-3">
                  <div className="min-w-0">
                    <div className="flex flex-wrap items-center gap-2 text-xs font-semibold text-ink">
                      <span>{log.channel.toUpperCase()}</span>
                      <Badge tone={log.complete ? "ok" : log.status === "recording" ? "brand" : "warn"}>
                        {log.status}
                      </Badge>
                      {log.pinned && <Pin size={12} className="text-brand" aria-label={t("serial.hostLogs.pinned")} />}
                    </div>
                    <div className="mt-1 font-mono text-[10px] text-ink-dim">
                      {new Date(log.started_at).toLocaleString()} · {formatBytes(log.bytes)}
                    </div>
                  </div>
                  <span className="shrink-0 font-mono text-[10px] text-ink-dim">
                    {log.baud.toLocaleString()} baud
                  </span>
                </div>

                <div className={`mt-2 rounded-lg px-2.5 py-2 text-[10px] ${log.complete ? "bg-panel2/60 text-ink-dim" : "bg-warn/10 text-warn"}`}>
                  <div className="flex flex-wrap gap-x-3 gap-y-1 font-mono">
                    <span>
                      {t("serial.hostLogs.records").replaceAll(
                        "{count}",
                        log.records.toLocaleString()
                      )}
                    </span>
                    <span>
                      {t("serial.hostLogs.segments").replaceAll(
                        "{count}",
                        log.segments.toLocaleString()
                      )}
                    </span>
                    <span>
                      {t("serial.hostLogs.droppedBytes").replaceAll(
                        "{bytes}",
                        formatBytes(log.dropped_bytes)
                      )}
                    </span>
                  </div>
                  {!log.complete && log.end_reason && (
                    <p className="mt-1 break-words">{log.end_reason}</p>
                  )}
                </div>

                <div className="mt-2 flex flex-wrap items-center justify-between gap-2 border-t border-line/50 pt-2">
                  <div className="flex flex-wrap items-center gap-1">
                    {(["raw", "text", "ndjson"] as const).map((format) => (
                      <a
                        key={format}
                        href={hostSerialLogDownloadUrl(log.session_id, format)}
                        download
                        className="inline-flex min-h-8 items-center gap-1 rounded-lg px-2 text-[10px] font-medium uppercase text-ink-dim transition-colors hover:bg-panel2 hover:text-ink focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40"
                        title={t(`serial.hostLogs.download.${format}`)}
                      >
                        <Download size={12} /> {format}
                      </a>
                    ))}
                  </div>
                  <div className="flex items-center gap-1">
                    <Button
                      type="button"
                      variant="ghost"
                      className="h-8 min-h-8 w-8 px-0"
                      disabled={log.status === "recording" || workingId === log.session_id}
                      title={log.pinned ? t("serial.hostLogs.unpin") : t("serial.hostLogs.pin")}
                      aria-label={log.pinned ? t("serial.hostLogs.unpin") : t("serial.hostLogs.pin")}
                      onClick={() => void togglePinned(log)}
                    >
                      {log.pinned ? <PinOff size={14} /> : <Pin size={14} />}
                    </Button>
                    <Button
                      type="button"
                      variant="ghost"
                      className="h-8 min-h-8 w-8 px-0 text-danger"
                      disabled={log.status === "recording" || log.pinned || workingId === log.session_id}
                      title={t("serial.hostLogs.delete")}
                      aria-label={t("serial.hostLogs.delete")}
                      onClick={() => void remove(log)}
                    >
                      <Trash2 size={14} />
                    </Button>
                  </div>
                </div>
              </li>
            ))}
          </ul>
        )}
      </div>
    </section>
  );
}
