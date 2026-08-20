import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ChangeEvent,
  type Dispatch,
  type SetStateAction,
} from "react";
import {
  CheckCircle2,
  Clock3,
  FileArchive,
  Loader2,
  RefreshCw,
  ShieldCheck,
  TriangleAlert,
  Upload,
} from "lucide-react";
import { Badge, Button, Card, Stat } from "./ui";
import { useI18n } from "@/lib/i18n";
import { formatBytes } from "@/lib/utils";
import type { AutomationTaskControl } from "@/lib/automationTask";
import {
  OTA_AUTO_CONFIRM_MS,
  canConfirmOta,
  canStartOtaTest,
  canUploadOta,
  computeOtaSha256Hex,
  confirmOtaImage,
  formatOtaFirmwareError,
  getOtaStatus,
  isOtaRebooting,
  startOtaTest,
  uploadOtaImage,
  type OtaStatus,
  type OtaUploadProgress,
} from "@/lib/ota";

const POLL_INTERVAL_MS = 1500;
const REBOOT_GRACE_MS = 25_000;

type BusyAction = "hashing" | "uploading" | "testing" | "confirming" | null;

function replaceTokens(
  template: string,
  values: Record<string, string | number>
): string {
  return Object.entries(values).reduce(
    (text, [key, value]) => text.replaceAll(`{${key}}`, String(value)),
    template
  );
}

function statusTone(status: OtaStatus | null): "neutral" | "warn" | "brand" | "danger" | "ok" {
  if (!status) {
    return "neutral";
  }

  switch (status.state) {
    case "verified":
      return "ok";
    case "pending_test":
    case "rebooting":
      return "warn";
    case "uploading":
      return "brand";
    case "failed":
    case "unknown":
      return "danger";
    default:
      return "neutral";
  }
}

function lifecycleIndex(status: OtaStatus | null): number {
  if (status?.state === "idle" && status.currentImageConfirmed === true) return 4;
  switch (status?.state) {
    case "pending_test": return 3;
    case "rebooting": return 2;
    case "verified": return 1;
    default: return 0;
  }
}

export function OtaCard({
  status,
  setStatus,
  disabled = false,
  taskControl,
}: {
  status: OtaStatus | null;
  setStatus: Dispatch<SetStateAction<OtaStatus | null>>;
  disabled?: boolean;
  taskControl?: AutomationTaskControl;
}) {
  const { t } = useI18n();
  const fileInputRef = useRef<HTMLInputElement | null>(null);
  const selectButtonRef = useRef<HTMLButtonElement | null>(null);
  const pollTimerRef = useRef<number | null>(null);
  const requestInFlightRef = useRef(false);
  const rebootGraceUntilRef = useRef(0);
  const selectedJobRef = useRef(0);
  const previousConfirmedRef = useRef<boolean | null>(null);
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [pageVisible, setPageVisible] = useState(
    typeof document === "undefined" ? true : !document.hidden
  );
  const [busyAction, setBusyAction] = useState<BusyAction>(null);
  const [selectedFile, setSelectedFile] = useState<File | null>(null);
  const [selectedSha, setSelectedSha] = useState("");
  const [uploadProgress, setUploadProgress] = useState<OtaUploadProgress | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const [pollError, setPollError] = useState<string | null>(null);
  const [note, setNote] = useState<string | null>(null);
  const abortRef = useRef<AbortController | null>(null);
  const [rebootCountdown, setRebootCountdown] = useState<number | null>(null);
  const [isDragging, setIsDragging] = useState(false);

  const setLocalRebooting = useCallback(() => {
    rebootGraceUntilRef.current = Date.now() + REBOOT_GRACE_MS;
    setRebootCountdown(Math.round(REBOOT_GRACE_MS / 1000));
    setStatus((current) =>
      current ? { ...current, state: "rebooting" } : {
        state: "rebooting",
        expectedSize: null,
        writtenSize: null,
        maxSize: null,
        currentImageConfirmed: null,
        lastError: null,
      }
    );
  }, []);

  // Countdown timer for reboot
  useEffect(() => {
    if (rebootCountdown === null || rebootCountdown <= 0) return;
    const timer = window.setInterval(() => {
      setRebootCountdown((prev) => {
        if (prev === null || prev <= 1) {
          window.clearInterval(timer);
          return null;
        }
        return prev - 1;
      });
    }, 1000);
    return () => window.clearInterval(timer);
  }, [rebootCountdown]);

  const busy = busyAction !== null;
  const waitingForReboot = rebootGraceUntilRef.current > Date.now();

  const acquireOtaTask = () => {
    if (!taskControl || taskControl.acquire("ota")) return true;
    setActionError(t("task.error.busy"));
    return false;
  };

  useEffect(() => {
    const handleVisibility = () => {
      setPageVisible(!document.hidden);
    };

    document.addEventListener("visibilitychange", handleVisibility);
    return () => {
      document.removeEventListener("visibilitychange", handleVisibility);
    };
  }, []);

  const refreshStatus = useCallback(
    async (mode: "manual" | "poll" = "poll") => {
      if (requestInFlightRef.current) {
        return;
      }

      requestInFlightRef.current = true;
      if (mode === "manual") {
        setRefreshing(true);
      }

      try {
        const nextStatus = await getOtaStatus();
        const withinGrace = rebootGraceUntilRef.current > Date.now();
        const shouldHoldRebooting = withinGrace && nextStatus.state === "verified";

        if (!shouldHoldRebooting) {
          setStatus(nextStatus);
        }
        setPollError(null);
        if (rebootGraceUntilRef.current > 0 && (
          nextStatus.state === "pending_test" ||
          (nextStatus.state === "idle" && nextStatus.currentImageConfirmed === true)
        )) {
          rebootGraceUntilRef.current = 0;
          setRebootCountdown(null);
        }
      } catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        if (mode === "poll" && rebootGraceUntilRef.current > Date.now()) {
          setStatus((current) =>
            current ? { ...current, state: "rebooting" } : {
              state: "rebooting",
              expectedSize: null,
              writtenSize: null,
              maxSize: null,
              currentImageConfirmed: null,
              lastError: null,
            }
          );
        } else {
          setPollError(message);
        }
      } finally {
        requestInFlightRef.current = false;
        setLoading(false);
        setRefreshing(false);
      }
    },
    []
  );

  useEffect(() => {
    void refreshStatus("poll");
  }, [refreshStatus]);

  useEffect(() => {
    if (!pageVisible) {
      if (pollTimerRef.current != null) {
        window.clearTimeout(pollTimerRef.current);
        pollTimerRef.current = null;
      }
      return;
    }

    let cancelled = false;

    const loop = async () => {
      await refreshStatus("poll");
      if (!cancelled) {
        pollTimerRef.current = window.setTimeout(loop, POLL_INTERVAL_MS);
      }
    };

    pollTimerRef.current = window.setTimeout(loop, POLL_INTERVAL_MS);

    return () => {
      cancelled = true;
      if (pollTimerRef.current != null) {
        window.clearTimeout(pollTimerRef.current);
        pollTimerRef.current = null;
      }
    };
  }, [pageVisible, refreshStatus]);

  useEffect(() => {
    return () => {
      if (pollTimerRef.current != null) {
        window.clearTimeout(pollTimerRef.current);
      }
    };
  }, []);

  useEffect(() => {
    const previous = previousConfirmedRef.current;
    const current = status?.currentImageConfirmed ?? null;
    if (previous === false && current === true) {
      setNote(t("ota.confirmed.note"));
    }
    previousConfirmedRef.current = current;
  }, [status?.currentImageConfirmed, t]);

  const helperMessage = useMemo(() => {
    if (waitingForReboot || isOtaRebooting(status)) {
      return t("ota.reboot.waiting");
    }
    if (status?.state === "unknown") {
      return t("ota.unknown.note");
    }
    if (status?.state === "pending_test" && status.currentImageConfirmed === false) {
      return replaceTokens(t("ota.pending.note"), {
        seconds: Math.round(OTA_AUTO_CONFIRM_MS / 1000),
      });
    }
    return note || t("ota.note");
  }, [note, status, t, waitingForReboot]);

  const handleSelectClick = () => {
    fileInputRef.current?.click();
  };

  const handleFileChange = async (event: ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0] ?? null;
    event.target.value = "";

    if (!file) {
      return;
    }

    setActionError(null);
    setNote(null);
    setUploadProgress(null);

    if (!file.name.toLowerCase().endsWith(".bin")) {
      setSelectedFile(null);
      setSelectedSha("");
      setActionError(t("ota.file.invalidType"));
      selectButtonRef.current?.focus();
      return;
    }

    if (file.size === 0) {
      setSelectedFile(null);
      setSelectedSha("");
      setActionError(t("ota.file.empty"));
      selectButtonRef.current?.focus();
      return;
    }

    if (status?.maxSize != null && file.size > status.maxSize) {
      setSelectedFile(null);
      setSelectedSha("");
      setActionError(
        replaceTokens(t("ota.file.tooLarge"), {
          maxSize: formatBytes(status.maxSize),
        })
      );
      selectButtonRef.current?.focus();
      return;
    }

    const job = selectedJobRef.current + 1;
    selectedJobRef.current = job;
    setSelectedFile(file);
    setSelectedSha("");
    setBusyAction("hashing");

    try {
      const sha = await computeOtaSha256Hex(file);
      if (selectedJobRef.current !== job) {
        return;
      }
      setSelectedSha(sha);
      setNote(t("ota.file.ready"));
    } catch (error) {
      if (selectedJobRef.current !== job) {
        return;
      }
      setSelectedFile(null);
      setSelectedSha("");
      setActionError(
        `${t("ota.file.hashFailed")}${error instanceof Error ? ` ${error.message}` : ` ${String(error)}`}`
      );
      selectButtonRef.current?.focus();
    } finally {
      if (selectedJobRef.current === job) {
        setBusyAction(null);
      }
    }
  };

  const handleUpload = async () => {
    if (disabled || !selectedFile || !selectedSha) {
      return;
    }
    if (!acquireOtaTask()) return;

    const controller = new AbortController();
    abortRef.current = controller;
    setActionError(null);
    setNote(null);
    setBusyAction("uploading");
    setUploadProgress({ loaded: 0, total: selectedFile.size, percent: 0 });

    try {
      const nextStatus = await uploadOtaImage(selectedFile, selectedSha, {
        onProgress: setUploadProgress,
        signal: controller.signal,
      });
      setStatus(nextStatus);
      setUploadProgress({
        loaded: selectedFile.size,
        total: selectedFile.size,
        percent: 100,
      });
      setNote(t("ota.upload.done"));
    } catch (error) {
      if (controller.signal.aborted) {
        setNote(t("ota.upload.cancelled"));
      } else {
        setActionError(error instanceof Error ? error.message : String(error));
      }
    } finally {
      if (abortRef.current === controller) {
        abortRef.current = null;
      }
      setBusyAction(null);
      taskControl?.release("ota");
    }
  };

  const handleCancelUpload = useCallback(() => {
    abortRef.current?.abort();
  }, []);

  const handleTestBoot = async () => {
    if (disabled) return;
    if (!window.confirm(t("ota.test.confirm"))) {
      return;
    }
    if (!acquireOtaTask()) return;

    setActionError(null);
    setNote(t("ota.test.accepted"));
    setBusyAction("testing");

    try {
      await startOtaTest();
      setLocalRebooting();
    } catch (error) {
      setActionError(error instanceof Error ? error.message : String(error));
      setNote(null);
    } finally {
      setBusyAction(null);
      taskControl?.release("ota");
    }
  };

  const handleConfirm = async () => {
    if (disabled) return;
    if (!acquireOtaTask()) return;
    setActionError(null);
    setNote(null);
    setBusyAction("confirming");

    try {
      await confirmOtaImage();
      setNote(t("ota.confirm.done"));
      await refreshStatus("manual");
    } catch (error) {
      setActionError(error instanceof Error ? error.message : String(error));
    } finally {
      setBusyAction(null);
      taskControl?.release("ota");
    }
  };

  const currentImageText =
    status?.currentImageConfirmed == null
      ? t("ota.currentImage.unknown")
      : status.currentImageConfirmed
        ? t("ota.currentImage.confirmed")
        : t("ota.currentImage.unconfirmed");
  const currentLifecycleIndex = lifecycleIndex(status);
  const lifecycleSteps = [
    "ota.lifecycle.upload",
    "ota.lifecycle.verify",
    "ota.lifecycle.test",
    "ota.lifecycle.confirm",
    "ota.lifecycle.complete",
  ] as const;

  const handleDragOver = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    if (!busy) {
      setIsDragging(true);
    }
  }, [busy]);

  const handleDragLeave = useCallback(() => {
    setIsDragging(false);
  }, []);

  const handleDrop = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    setIsDragging(false);

    if (busy) {
      return;
    }

    const file = e.dataTransfer.files[0];
    setActionError(null);
    setNote(null);
    setUploadProgress(null);

    if (!file || !file.name.toLowerCase().endsWith(".bin")) {
      setSelectedFile(null);
      setSelectedSha("");
      setActionError(t("ota.file.invalidType"));
      return;
    }

    // Trigger the same file processing as handleFileChange.
    void (async () => {
      if (file.size === 0) {
        setSelectedFile(null);
        setSelectedSha("");
        setActionError(t("ota.file.empty"));
        return;
      }

      if (status?.maxSize != null && file.size > status.maxSize) {
        setSelectedFile(null);
        setSelectedSha("");
        setActionError(replaceTokens(t("ota.file.tooLarge"), { maxSize: formatBytes(status.maxSize) }));
        return;
      }

      const job = selectedJobRef.current + 1;
      selectedJobRef.current = job;
      setSelectedFile(file);
      setSelectedSha("");
      setBusyAction("hashing");

      try {
        const sha = await computeOtaSha256Hex(file);
        if (selectedJobRef.current !== job) return;
        setSelectedSha(sha);
        setNote(t("ota.file.ready"));
      } catch (error) {
        if (selectedJobRef.current !== job) return;
        setSelectedFile(null);
        setSelectedSha("");
        setActionError(`${t("ota.file.hashFailed")}${error instanceof Error ? ` ${error.message}` : ` ${String(error)}`}`);
      } finally {
        if (selectedJobRef.current === job) setBusyAction(null);
      }
    })();
  }, [busy, status?.maxSize, t]);

  return (
    <div
      className={`min-w-0 ${isDragging ? "ring-2 ring-brand/50 rounded-2xl" : ""}`}
      aria-busy={loading || busy}
      onDragOver={handleDragOver}
      onDragLeave={handleDragLeave}
      onDrop={handleDrop}
    >
      <Card
        title={t("ota.title")}
        subtitle={t("ota.subtitle")}
        icon={ShieldCheck}
        right={disabled ? <Badge tone="neutral">{t("ota.actionsLocked")}</Badge> : undefined}
      >
        <div className="flex flex-wrap items-center gap-2">
          <Badge tone={statusTone(status)}>
            {loading ? <Loader2 size={12} className="animate-spin" /> : <Clock3 size={12} />}
             {loading ? t("loading") : t(`ota.state.${status?.state ?? "unknown"}`)}
          </Badge>
          <Button
            type="button"
            variant="ghost"
            className="min-h-8 px-2 py-1 text-xs"
            onClick={() => void refreshStatus("manual")}
            disabled={refreshing || busyAction === "uploading"}
            aria-label={t("ota.refresh")}
            title={t("ota.refresh")}
          >
            <RefreshCw size={13} className={refreshing ? "animate-spin" : ""} />
            {t("ota.refresh")}
          </Button>
        </div>

        <ol className="mt-3 grid grid-cols-5 overflow-hidden rounded-xl border border-line/70 bg-panel2/30" aria-label={t("ota.lifecycle.label")}>
          {lifecycleSteps.map((key, index) => {
            const current = index === currentLifecycleIndex;
            const complete = index < currentLifecycleIndex || (currentLifecycleIndex === 4 && index === 4);
            return (
              <li
                key={key}
                aria-current={current ? "step" : undefined}
                className={`min-w-0 border-l border-line/60 px-1.5 py-2 text-center first:border-l-0 ${current ? "bg-brand/10" : ""}`}
              >
                <span className={`mx-auto block h-1.5 w-1.5 rounded-full ${complete ? "bg-ok" : current ? "bg-brand" : "bg-line"}`} />
                <span className={`mt-1 block truncate text-[10px] font-medium ${current || complete ? "text-ink" : "text-ink-dim"}`}>{t(key)}</span>
              </li>
            );
          })}
        </ol>

        <div className="mt-3 rounded-lg border border-warn/30 bg-warn/5 px-3 py-2 text-xs leading-5 text-ink-dim">
          <TriangleAlert size={13} className="mr-1 inline text-warn" />
          {t("ota.safety")}
        </div>

        <div className="mt-3 grid gap-3">
          <div className="min-w-0 rounded-xl border border-line/70 bg-panel2/35 p-3">
            <div className="flex flex-wrap items-center gap-2">
              <span className="text-xs font-semibold text-ink">{t("ota.file.label")}</span>
              <span className="text-[11px] text-ink-dim">{t("ota.file.hint")}</span>
            </div>

            <div className="mt-3 flex flex-wrap gap-2">
              <input
                ref={fileInputRef}
                type="file"
                accept=".bin,application/octet-stream"
                className="sr-only"
                onChange={(event) => void handleFileChange(event)}
                aria-label={t("ota.file.label")}
              />
              <Button
                ref={selectButtonRef}
                type="button"
                variant="default"
                className="min-w-[9rem]"
                onClick={handleSelectClick}
                disabled={busy}
              >
                <FileArchive size={15} />
                {selectedFile ? t("ota.file.replace") : t("ota.file.select")}
              </Button>
              <Button
                type="button"
                variant="primary"
                className="min-w-[9rem]"
                onClick={() => void handleUpload()}
                disabled={disabled || !selectedFile || !selectedSha || !canUploadOta(status, busy)}
              >
                {busyAction === "uploading" ? (
                  <Loader2 size={15} className="animate-spin" />
                ) : (
                  <Upload size={15} />
                )}
                {busyAction === "uploading" ? t("ota.uploading") : t("ota.upload")}
              </Button>
            </div>

            <div className="mt-3 rounded-lg border border-line/60 bg-panel px-3 py-3">
              {selectedFile ? (
                <div className="space-y-2">
                  <div>
                    <div className="text-[10px] text-ink-dim">{t("ota.file.name")}</div>
                    <div className="truncate text-sm font-medium text-ink">{selectedFile.name}</div>
                  </div>
                  <div className="grid gap-2 sm:grid-cols-2">
                    <div>
                      <div className="text-[10px] text-ink-dim">{t("ota.file.size")}</div>
                      <div className="font-mono text-xs text-ink">{formatBytes(selectedFile.size)}</div>
                    </div>
                    <div>
                      <div className="text-[10px] text-ink-dim">{t("ota.file.sha256")}</div>
                      <div className="font-mono text-xs text-ink break-all">
                        {busyAction === "hashing" ? t("ota.file.hashing") : selectedSha || "—"}
                      </div>
                    </div>
                  </div>
                </div>
              ) : (
                <div className="text-sm text-ink-dim">{t("ota.file.none")}</div>
              )}
            </div>

            {uploadProgress && (
              <div className="mt-3" aria-live="polite">
                <div className="flex items-center justify-between gap-3 text-[11px] text-ink-dim">
                  <span>{t("ota.progress")}</span>
                  <span className="font-mono">
                    {uploadProgress.percent}% · {formatBytes(uploadProgress.loaded)} / {formatBytes(uploadProgress.total)}
                  </span>
                </div>
                <div className="mt-1 h-2 overflow-hidden rounded-full bg-panel">
                  <div
                    className="h-full rounded-full bg-brand transition-[width] duration-150"
                    style={{ width: `${uploadProgress.percent}%` }}
                  />
                </div>
                {busyAction === "uploading" && (
                  <div className="mt-2">
                    <Button variant="ghost" onClick={handleCancelUpload} className="text-xs">
                      {t("ota.cancelUpload")}
                    </Button>
                  </div>
                )}
              </div>
            )}
          </div>

          <div className="min-w-0 space-y-3">
            <div className="grid gap-2 sm:grid-cols-2">
              <Stat label={t("ota.expectedSize")} value={<span className="font-mono text-sm">{formatBytes(status?.expectedSize ?? undefined)}</span>} />
              <Stat label={t("ota.writtenSize")} value={<span className="font-mono text-sm">{formatBytes(status?.writtenSize ?? undefined)}</span>} />
              <Stat label={t("ota.maxSize")} value={<span className="font-mono text-sm">{formatBytes(status?.maxSize ?? undefined)}</span>} />
              <Stat label={t("ota.currentImage.label")} value={<span className="text-sm">{currentImageText}</span>} />
            </div>

            <div className="flex flex-wrap gap-2">
              <Button
                type="button"
                className="min-w-[9rem]"
                onClick={() => void handleTestBoot()}
                disabled={disabled || !canStartOtaTest(status, busy)}
              >
                {busyAction === "testing" ? (
                  <Loader2 size={15} className="animate-spin" />
                ) : (
                  <Clock3 size={15} />
                )}
                {busyAction === "testing" ? t("ota.testing") : t("ota.test")}
              </Button>
              <Button
                type="button"
                variant="default"
                className="min-w-[9rem]"
                onClick={() => void handleConfirm()}
                disabled={disabled || !canConfirmOta(status, busy)}
              >
                {busyAction === "confirming" ? (
                  <Loader2 size={15} className="animate-spin" />
                ) : (
                  <CheckCircle2 size={15} />
                )}
                {busyAction === "confirming" ? t("ota.confirming") : t("ota.confirm")}
              </Button>
            </div>

            {status?.lastError && (
                <div className="rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
                  <div className="font-medium">{t("ota.lastError")}</div>
                  <div className="mt-1 break-words">
                    {formatOtaFirmwareError(status.lastError)}
                  </div>
                </div>
              )}
          </div>
        </div>

        <div className="mt-3 space-y-2" aria-live="polite">
          {actionError && (
            <div className="rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
              <TriangleAlert size={13} className="mr-1 inline" />
              {actionError}
            </div>
          )}
          {pollError && (
            <div className="rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
              <TriangleAlert size={13} className="mr-1 inline" />
              {pollError}
            </div>
          )}
          <div className="rounded-lg border border-line/60 bg-panel2/25 px-3 py-2 text-xs text-ink-dim">
            {helperMessage}
          </div>
          {rebootCountdown !== null && rebootCountdown > 0 && (
            <div className="mt-2 flex items-center gap-2 text-xs text-ink-dim">
              <Loader2 size={14} className="animate-spin text-brand" />
              <span>{t("ota.rebootCountdown", { seconds: rebootCountdown })}</span>
              <div className="h-1.5 flex-1 overflow-hidden rounded-full bg-panel">
                <div
                  className="h-full bg-brand transition-all duration-1000"
                  style={{ width: `${(rebootCountdown / Math.round(REBOOT_GRACE_MS / 1000)) * 100}%` }}
                />
              </div>
            </div>
          )}
          {isDragging && (
            <div className="mt-2 rounded-lg border-2 border-dashed border-brand/50 bg-brand/5 px-3 py-4 text-center text-xs text-brand">
              {t("ota.dropActive")}
            </div>
          )}
        </div>
      </Card>
    </div>
  );
}
