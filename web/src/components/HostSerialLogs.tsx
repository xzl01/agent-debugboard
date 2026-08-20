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
      ? "danger"
      : status.active_sessions > 0
        ? "ok"
        : "brand";

  return (
    <details id="serial-logs" className="group mt-3 rounded-xl border border-line/60 bg-panel2/30">
      <summary className="flex min-h-12 cursor-pointer list-none items-center gap-3 px-3 py-2 outline-none focus-visible:ring-2 focus-visible:ring-brand/40">
        <Archive size={16} className="shrink-0 text-brand" aria-hidden="true" />
        <span className="min-w-0 flex-1">
          <span className="block text-xs font-semibold text-ink">{t("serial.hostLogs.title")}</span>
          <span className="block truncate text-[11px] text-ink-dim">
            {status?.enabled
              ? t("serial.hostLogs.summary")
                  .replaceAll("{used}", formatBytes(status.total_bytes))
                  .replaceAll("{quota}", formatBytes(status.quota_bytes))
              : t("serial.hostLogs.unavailable")}
          </span>
        </span>
        <Badge tone={tone}>
          {status?.active_sessions
            ? t("serial.hostLogs.recording")
            : status?.enabled
              ? t("serial.hostLogs.ready")
              : t("serial.hostLogs.off")}
        </Badge>
      </summary>

      <div className="space-y-3 border-t border-line/60 p-3">
        <div className="flex flex-wrap items-center justify-between gap-2 text-[11px] text-ink-dim">
          <span>{t("serial.hostLogs.bridgeOnly")}</span>
          <Button
            type="button"
            variant="ghost"
            className="min-h-8 px-2 py-1 text-xs"
            disabled={loading}
            onClick={() => void refresh()}
          >
            <RefreshCw size={14} className={loading ? "animate-spin" : ""} />
            {t("serial.hostLogs.refresh")}
          </Button>
        </div>

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
              <li key={log.session_id} className="flex flex-wrap items-center gap-3 rounded-lg border border-line/60 bg-panel px-3 py-2">
                <div className="min-w-0 flex-1">
                  <div className="flex flex-wrap items-center gap-2 text-xs font-medium text-ink">
                    <span>{log.channel.toUpperCase()}</span>
                    <Badge tone={log.complete ? "ok" : log.status === "recording" ? "brand" : "warn"}>
                      {log.status}
                    </Badge>
                    {log.pinned && <Pin size={12} className="text-brand" aria-label={t("serial.hostLogs.pinned")} />}
                  </div>
                  <div className="mt-1 truncate font-mono text-[10px] text-ink-dim">
                    {new Date(log.started_at).toLocaleString()} · {formatBytes(log.bytes)} · {log.baud.toLocaleString()} baud
                  </div>
                </div>
                <div className="flex items-center gap-1">
                  {(["raw", "text", "ndjson"] as const).map((format) => (
                    <a
                      key={format}
                      href={hostSerialLogDownloadUrl(log.session_id, format)}
                      download
                      className="inline-flex min-h-8 items-center gap-1 rounded-lg px-2 text-[11px] font-medium text-ink-dim transition-colors hover:bg-panel2 hover:text-ink focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40"
                      title={t(`serial.hostLogs.download.${format}`)}
                    >
                      <Download size={12} /> {format}
                    </a>
                  ))}
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
              </li>
            ))}
          </ul>
        )}
      </div>
    </details>
  );
}
