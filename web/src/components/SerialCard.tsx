import {
  forwardRef,
  useCallback,
  useEffect,
  useImperativeHandle,
  useRef,
  useState,
  type MouseEvent,
} from "react";
import {
  Check,
  Columns2,
  Copy,
  LayoutPanelTop,
  ShieldAlert,
  Terminal as TerminalIcon,
  Usb,
  X,
} from "lucide-react";
import { Button, Card } from "./ui";
import {
  SerialTerminalPane,
  type SerialChannelHandle,
  type SerialChannelId,
  type SerialChannelStatus,
} from "./SerialTerminalPane";
import { useI18n } from "@/lib/i18n";

const CH347_VID = 0x1a86;
const CHANNELS: SerialChannelId[] = ["uart0", "uart1"];
const BOARD_HTTP_ORIGIN = "http://172.29.203.1:8080";
const CHROMIUM_FLAG_URL = "chrome://flags/#unsafely-treat-insecure-origin-as-secure";
const FOCUSABLE_SELECTOR = [
  "button:not([disabled])",
  "[href]",
  'input:not([disabled]):not([type="hidden"])',
  "select:not([disabled])",
  "textarea:not([disabled])",
  '[tabindex]:not([tabindex="-1"])',
].join(", ");
const SETUP_COPY_BUTTON_CLASS =
  "w-full overflow-hidden rounded-md border border-line/70 bg-panel2 px-3 py-2 text-left font-mono text-[11px] text-ink break-all transition-colors hover:border-brand/40 hover:bg-brand/5 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40";

type LayoutMode = "tabs" | "split";
type CopyState = "idle" | "success" | "error";
type CopyTarget = "flag" | "origin";

const EMPTY_STATUS: SerialChannelStatus = {
  connected: false,
  connecting: false,
  source: null,
  portInfo: "",
  rxBytes: 0,
  txBytes: 0,
};

export type { SerialChannelId } from "./SerialTerminalPane";

export interface SerialAutomationHandle {
  isConnected: (channel?: SerialChannelId) => boolean;
  connectedChannels: () => SerialChannelId[];
  clear: (channel?: SerialChannelId) => void;
  subscribe: (
    listener: (text: string, receivedAtMs: number) => void,
    channel?: SerialChannelId
  ) => () => void;
}

export const SerialCard = forwardRef<
  SerialAutomationHandle,
  {
    vinRoute?: string;
    onSetVin: (route: "1.8v" | "3.3v") => Promise<void>;
  }
>(function SerialCard({ vinRoute, onSetVin }, automationRef) {
  const { t } = useI18n();
  const isSecureContext = typeof window !== "undefined" && window.isSecureContext;
  const hasWebSerialApi = typeof navigator !== "undefined" && "serial" in navigator;
  const isBoardHttpOrigin =
    typeof window !== "undefined" && window.location.origin === BOARD_HTTP_ORIGIN;
  const webSerialSupported = isSecureContext && hasWebSerialApi;
  const needsInsecureOriginSetup = isBoardHttpOrigin && !isSecureContext;

  const [activeChannel, setActiveChannel] = useState<SerialChannelId>("uart0");
  const [layout, setLayout] = useState<LayoutMode>("tabs");
  const [changingVoltage, setChangingVoltage] = useState(false);
  const [sharedError, setSharedError] = useState<string | null>(null);
  const [showSetupModal, setShowSetupModal] = useState(false);
  const [copyState, setCopyState] = useState<Record<CopyTarget, CopyState>>({
    flag: "idle",
    origin: "idle",
  });
  const [statuses, setStatuses] = useState<Record<SerialChannelId, SerialChannelStatus>>({
    uart0: EMPTY_STATUS,
    uart1: EMPTY_STATUS,
  });

  const activeChannelRef = useRef<SerialChannelId>("uart0");
  const uart0Ref = useRef<SerialChannelHandle>(null);
  const uart1Ref = useRef<SerialChannelHandle>(null);
  const assignedPortsRef = useRef(new Map<SerialChannelId, SerialPort>());
  const dialogRef = useRef<HTMLDivElement | null>(null);
  const restoreFocusRef = useRef<HTMLElement | null>(null);
  const previousBodyOverflowRef = useRef<string | null>(null);

  const channelHandle = (channel: SerialChannelId) =>
    channel === "uart0" ? uart0Ref.current : uart1Ref.current;

  const selectChannel = (channel: SerialChannelId) => {
    activeChannelRef.current = channel;
    setActiveChannel(channel);
  };

  const onStatus = useCallback((channel: SerialChannelId, status: SerialChannelStatus) => {
    setStatuses((current) =>
      current[channel] === status ? current : { ...current, [channel]: status }
    );
  }, []);

  const requestPort = useCallback(
    async (channel: SerialChannelId) => {
      const assigned = assignedPortsRef.current.get(channel);
      if (assigned) return assigned;

      const ports = (await navigator.serial.getPorts()).filter(
        (candidate) => candidate.getInfo().usbVendorId === CH347_VID
      );
      const portsInUse = new Set(assignedPortsRef.current.values());
      const available = ports.filter((candidate) => !portsInUse.has(candidate));
      const port =
        available.length === 1
          ? available[0]
          : await navigator.serial.requestPort({ filters: [{ usbVendorId: CH347_VID }] });
      const conflictingChannel = CHANNELS.find(
        (candidate) =>
          candidate !== channel && assignedPortsRef.current.get(candidate) === port
      );
      if (conflictingChannel) {
        throw new Error(
          t("serial.portInUse").replaceAll("{channel}", conflictingChannel.toUpperCase())
        );
      }
      assignedPortsRef.current.set(channel, port);
      return port;
    },
    [t]
  );

  const releasePort = useCallback(
    (channel: SerialChannelId, port: SerialPort, physicalDisconnect: boolean) => {
      if (physicalDisconnect && assignedPortsRef.current.get(channel) === port) {
        assignedPortsRef.current.delete(channel);
      }
    },
    []
  );

  useImperativeHandle(
    automationRef,
    () => ({
      isConnected: (channel = activeChannelRef.current) =>
        channelHandle(channel)?.isConnected() ?? false,
      connectedChannels: () =>
        CHANNELS.filter((channel) => channelHandle(channel)?.isConnected()),
      clear: (channel = activeChannelRef.current) => channelHandle(channel)?.clear(),
      subscribe: (listener, channel = activeChannelRef.current) =>
        channelHandle(channel)?.subscribe(listener) ?? (() => {}),
    }),
    []
  );

  useEffect(() => {
    setCopyState({ flag: "idle", origin: "idle" });
  }, [showSetupModal]);

  useEffect(() => {
    if (!showSetupModal || typeof window === "undefined" || typeof document === "undefined") {
      return;
    }

    const dialog = dialogRef.current;
    if (!dialog) return;

    const activeElement = document.activeElement;
    if (
      !restoreFocusRef.current &&
      activeElement instanceof HTMLElement &&
      !dialog.contains(activeElement)
    ) {
      restoreFocusRef.current = activeElement;
    }

    previousBodyOverflowRef.current = document.body.style.overflow;
    document.body.style.overflow = "hidden";

    const getFocusableElements = () =>
      Array.from(dialog.querySelectorAll<HTMLElement>(FOCUSABLE_SELECTOR)).filter(
        (element) => !element.hasAttribute("disabled") && element.tabIndex !== -1
      );

    const focusableElements = getFocusableElements();
    const firstFocusable = focusableElements[0];
    const closeButton = dialog.querySelector<HTMLElement>('button[aria-label]');
    (closeButton ?? firstFocusable ?? dialog).focus();

    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") {
        setShowSetupModal(false);
        return;
      }

      if (event.key !== "Tab") return;

      const nextFocusable = getFocusableElements();
      if (nextFocusable.length === 0) {
        event.preventDefault();
        dialog.focus();
        return;
      }

      const first = nextFocusable[0];
      const last = nextFocusable[nextFocusable.length - 1];
      const current = document.activeElement;

      if (!dialog.contains(current)) {
        event.preventDefault();
        first.focus();
        return;
      }

      if (event.shiftKey && current === first) {
        event.preventDefault();
        last.focus();
        return;
      }

      if (!event.shiftKey && current === last) {
        event.preventDefault();
        first.focus();
      }
    };

    window.addEventListener("keydown", onKeyDown);
    return () => {
      window.removeEventListener("keydown", onKeyDown);
      document.body.style.overflow = previousBodyOverflowRef.current ?? "";
      previousBodyOverflowRef.current = null;
      restoreFocusRef.current?.focus();
      restoreFocusRef.current = null;
    };
  }, [showSetupModal]);

  async function changeVin(next: "1.8v" | "3.3v") {
    if (next === vinRoute) return;

    const current =
      vinRoute === "1.8v" || vinRoute === "3.3v" ? vinRoute : t("serial.vioUnknown");
    const connected = CHANNELS.filter((channel) => statuses[channel].connected);
    const confirmed = window.confirm(
      t("serial.vioConfirm")
        .replaceAll("{current}", current)
        .replaceAll("{next}", next)
        .replaceAll(
          "{channels}",
          connected.length > 0
            ? connected.map((channel) => channel.toUpperCase()).join(" / ")
            : t("serial.none")
        )
    );
    if (!confirmed) return;

    setChangingVoltage(true);
    setSharedError(null);
    try {
      await Promise.all(CHANNELS.map((channel) => channelHandle(channel)?.disconnect()));
      await onSetVin(next);
    } catch (reason) {
      setSharedError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setChangingVoltage(false);
    }
  }

  async function copyValue(value: string, target: CopyTarget) {
    if (typeof window === "undefined" || typeof document === "undefined") {
      setCopyState((prev) => ({ ...prev, [target]: "error" }));
      return;
    }

    if (
      window.isSecureContext &&
      typeof navigator !== "undefined" &&
      navigator.clipboard?.writeText
    ) {
      try {
        await navigator.clipboard.writeText(value);
        setCopyState((prev) => ({ ...prev, [target]: "success" }));
        return;
      } catch (error) {
        console.error("Clipboard write failed", error);
      }
    }

    const textArea = document.createElement("textarea");
    const previousFocus =
      document.activeElement instanceof HTMLElement ? document.activeElement : null;
    textArea.value = value;
    textArea.setAttribute("readonly", "");
    textArea.style.position = "fixed";
    textArea.style.top = "0";
    textArea.style.left = "-9999px";
    textArea.style.opacity = "0";
    document.body.appendChild(textArea);

    try {
      textArea.focus();
      textArea.select();
      textArea.setSelectionRange(0, textArea.value.length);
      const copied = document.execCommand("copy");
      setCopyState((prev) => ({ ...prev, [target]: copied ? "success" : "error" }));
    } catch (error) {
      console.error("Fallback copy failed", error);
      setCopyState((prev) => ({ ...prev, [target]: "error" }));
    } finally {
      textArea.remove();
      previousFocus?.focus();
    }
  }

  function openSetupModal(event: MouseEvent<HTMLButtonElement>) {
    restoreFocusRef.current = event.currentTarget;
    setShowSetupModal(true);
  }

  const connectedCount = CHANNELS.filter((channel) => statuses[channel].connected).length;
  const serialNotice = needsInsecureOriginSetup
    ? t("serial.insecureOrigin")
    : isBoardHttpOrigin && webSerialSupported
      ? t("serial.insecureOriginActive")
      : webSerialSupported
        ? t("serial.webSerialHint")
        : t("serial.noWebSerial");
  const showSerialNotice = needsInsecureOriginSetup || isBoardHttpOrigin || !webSerialSupported;

  return (
    <>
      <Card
        title={t("serial.title")}
        subtitle={t("serial.subtitle")}
        icon={TerminalIcon}
        contentClassName="flex min-h-0 flex-col"
        right={
          <div className="flex max-w-full flex-wrap items-center justify-end gap-2">
            {layout === "tabs" && (
              <div
                className="inline-flex rounded-xl border border-line/70 bg-panel2 p-1"
                role="tablist"
                aria-label={t("serial.channels")}
              >
                {CHANNELS.map((channel) => {
                  const status = statuses[channel];
                  return (
                    <button
                      key={channel}
                      type="button"
                      role="tab"
                      aria-selected={activeChannel === channel}
                      onClick={() => selectChannel(channel)}
                      className={`flex min-h-8 items-center gap-2 rounded-lg px-3 text-xs font-semibold transition-colors ${
                        activeChannel === channel
                          ? "bg-panel text-ink shadow-sm"
                          : "text-ink-dim hover:text-ink"
                      }`}
                    >
                      <span
                        className={`h-2 w-2 rounded-full ${
                          status.connected
                            ? "bg-ok shadow-[0_0_7px_rgb(var(--c-ok))]"
                            : status.connecting
                              ? "animate-pulse bg-warn"
                              : "bg-ink-dim/40"
                        }`}
                      />
                      {channel.toUpperCase()}
                      {status.connected && (
                        <span className="font-mono text-[9px] font-normal text-ink-dim">
                          RX {status.rxBytes.toLocaleString()}
                        </span>
                      )}
                    </button>
                  );
                })}
              </div>
            )}
            {needsInsecureOriginSetup && (
              <Button
                variant="danger"
                className="min-h-9 rounded-xl px-3 py-2 text-xs"
                onClick={openSetupModal}
              >
                <Usb size={14} /> {t("serial.webSerial")}
              </Button>
            )}
            <div
              className="inline-flex rounded-xl border border-line/70 bg-panel2 p-1"
              role="group"
              aria-label={t("serial.layout")}
            >
              <button
                type="button"
                onClick={() => setLayout("tabs")}
                aria-pressed={layout === "tabs"}
                title={t("serial.layout.tabs")}
                className={`grid h-8 w-8 place-items-center rounded-lg transition-colors ${
                  layout === "tabs"
                    ? "bg-panel text-brand shadow-sm"
                    : "text-ink-dim hover:text-ink"
                }`}
              >
                <LayoutPanelTop size={15} />
              </button>
              <button
                type="button"
                onClick={() => setLayout("split")}
                aria-pressed={layout === "split"}
                title={t("serial.layout.split")}
                className={`grid h-8 w-8 place-items-center rounded-lg transition-colors ${
                  layout === "split"
                    ? "bg-panel text-brand shadow-sm"
                    : "text-ink-dim hover:text-ink"
                }`}
              >
                <Columns2 size={15} />
              </button>
            </div>
            <label
              className="inline-flex min-h-10 items-center gap-2 rounded-xl border border-warn/35 bg-warn/10 px-2.5 text-xs font-medium text-warn"
              title={t("serial.vioWarning")}
            >
              <ShieldAlert size={15} aria-hidden="true" />
              <span>VIO</span>
              <select
                aria-label={t("serial.vioLabel")}
                value={vinRoute === "1.8v" || vinRoute === "3.3v" ? vinRoute : ""}
                onChange={(event) =>
                  void changeVin(event.target.value as "1.8v" | "3.3v")
                }
                disabled={!vinRoute || changingVoltage}
                className="rounded-md border border-warn/30 bg-panel px-2 py-1 text-xs font-semibold text-ink outline-none focus-visible:ring-2 focus-visible:ring-warn/40 disabled:opacity-50"
              >
                <option value="" disabled>
                  —
                </option>
                <option value="3.3v">3.3V</option>
                <option value="1.8v">1.8V</option>
              </select>
            </label>
          </div>
        }
      >
        {showSerialNotice && (
          <div className="mb-3 flex flex-wrap items-start justify-between gap-3 rounded-lg border border-line/70 bg-panel2/50 px-3 py-2 text-xs text-ink-dim">
            <div className="min-w-0 flex-1">
              <p>{t("serial.connect")}</p>
              <p className="mt-1 break-all">{serialNotice}</p>
            </div>
            {needsInsecureOriginSetup && (
              <Button
                variant="danger"
                className="min-h-8 shrink-0 rounded-lg px-3 py-1 text-xs"
                onClick={openSetupModal}
              >
                <Usb size={14} /> {t("serial.webSerial")}
              </Button>
            )}
          </div>
        )}

        {sharedError && (
          <div className="mb-3 rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
            {sharedError}
          </div>
        )}

        <div className={layout === "split" ? "grid min-h-0 gap-3 lg:grid-cols-2" : "min-h-0"}>
          {CHANNELS.map((channel) => (
            <div
              key={channel}
              className={`min-h-0 min-w-0 ${
                layout === "tabs" && activeChannel !== channel ? "hidden" : "flex"
              }`}
              role={layout === "tabs" ? "tabpanel" : undefined}
            >
              <SerialTerminalPane
                ref={channel === "uart0" ? uart0Ref : uart1Ref}
                channel={channel}
                visible={layout === "split" || activeChannel === channel}
                compact={layout === "split"}
                webSerialSupported={webSerialSupported}
                requestPort={requestPort}
                releasePort={releasePort}
                onStatus={onStatus}
              />
            </div>
          ))}
        </div>

        <div className="mt-3 flex flex-wrap items-center justify-between gap-2 text-[11px] text-ink-dim">
          <span>
            {layout === "split"
              ? `${t("serial.connectedCount").replaceAll("{count}", String(connectedCount))} · ${t("serial.focusHint")}`
              : t("serial.dualHint")}
          </span>
        </div>
      </Card>

      {showSetupModal && (
        <div
          className="fixed inset-0 z-50 flex items-center justify-center bg-terminal/70 px-4 py-6"
          onClick={() => setShowSetupModal(false)}
        >
          <div
            role="dialog"
            aria-modal="true"
            aria-labelledby="serial-setup-title"
            tabIndex={-1}
            ref={dialogRef}
            className="max-h-[min(100%,42rem)] w-full max-w-lg overflow-auto rounded-xl border border-line/80 bg-panel shadow-2xl"
            onClick={(event) => event.stopPropagation()}
          >
            <div className="flex items-start justify-between gap-3 border-b border-line/60 px-4 py-3">
              <div className="min-w-0">
                <h3 id="serial-setup-title" className="text-sm font-semibold text-ink">
                  {t("serial.setupTitle")}
                </h3>
                <p className="mt-1 text-xs text-ink-dim">{t("serial.setupSummary")}</p>
              </div>
              <Button
                variant="ghost"
                type="button"
                aria-label={t("serial.setupClose")}
                title={t("serial.setupClose")}
                className="h-10 w-10 shrink-0 px-0"
                onClick={() => setShowSetupModal(false)}
              >
                <X size={16} />
              </Button>
            </div>

            <div className="space-y-4 px-4 py-4 text-xs text-ink-dim">
              <ol className="space-y-3">
                <li className="grid grid-cols-[1.5rem_minmax(0,1fr)] gap-3">
                  <div className="mt-0.5 grid h-6 w-6 place-items-center rounded-full border border-line/70 bg-panel2 text-[11px] font-semibold text-ink">
                    1
                  </div>
                  <div>
                    <div className="mb-1 font-medium text-ink">
                      {t("serial.setupStepFlagTitle")}
                    </div>
                    <button
                      type="button"
                      className={SETUP_COPY_BUTTON_CLASS}
                      title={t("serial.setupFlagCopyTitle")}
                      aria-label={t("serial.setupFlagCopyTitle")}
                      onClick={() => void copyValue(CHROMIUM_FLAG_URL, "flag")}
                    >
                      <span className="flex items-start gap-2">
                        {copyState.flag === "success" ? (
                          <Check size={16} className="mt-0.5 shrink-0 text-brand" />
                        ) : (
                          <Copy size={16} className="mt-0.5 shrink-0 text-ink-dim" />
                        )}
                        <span className="min-w-0 flex-1 break-all">{CHROMIUM_FLAG_URL}</span>
                      </span>
                    </button>
                    <p aria-live="polite" className="mt-2 text-[11px] text-ink-dim">
                      {copyState.flag === "success"
                        ? t("serial.setupFlagCopied")
                        : copyState.flag === "error"
                          ? t("serial.setupFlagCopyFailed")
                          : t("serial.setupFlagPasteHint")}
                    </p>
                  </div>
                </li>

                <li className="grid grid-cols-[1.5rem_minmax(0,1fr)] gap-3">
                  <div className="mt-0.5 grid h-6 w-6 place-items-center rounded-full border border-line/70 bg-panel2 text-[11px] font-semibold text-ink">
                    2
                  </div>
                  <div>
                    <div className="mb-1 font-medium text-ink">
                      {t("serial.setupStepOriginTitle")}
                    </div>
                    <button
                      type="button"
                      className={SETUP_COPY_BUTTON_CLASS}
                      title={t("serial.setupOriginCopyTitle")}
                      aria-label={t("serial.setupOriginCopyTitle")}
                      onClick={() => void copyValue(BOARD_HTTP_ORIGIN, "origin")}
                    >
                      <span className="flex items-start gap-2">
                        {copyState.origin === "success" ? (
                          <Check size={16} className="mt-0.5 shrink-0 text-brand" />
                        ) : (
                          <Copy size={16} className="mt-0.5 shrink-0 text-ink-dim" />
                        )}
                        <span className="min-w-0 flex-1 break-all">{BOARD_HTTP_ORIGIN}</span>
                      </span>
                    </button>
                    <p aria-live="polite" className="mt-2 text-[11px] text-ink-dim">
                      {copyState.origin === "success"
                        ? t("serial.setupOriginCopied")
                        : copyState.origin === "error"
                          ? t("serial.setupOriginCopyFailed")
                          : t("serial.setupOriginPasteHint")}
                    </p>
                  </div>
                </li>

                <li className="grid grid-cols-[1.5rem_minmax(0,1fr)] gap-3">
                  <div className="mt-0.5 grid h-6 w-6 place-items-center rounded-full border border-line/70 bg-panel2 text-[11px] font-semibold text-ink">
                    3
                  </div>
                  <div>
                    <div className="font-medium text-ink">{t("serial.setupStepEnableTitle")}</div>
                    <p className="mt-1">{t("serial.setupStepEnableBody")}</p>
                  </div>
                </li>
              </ol>

              <div className="space-y-2 rounded-md border border-line/70 bg-panel2/60 px-3 py-3">
                <p>{t("serial.setupChooser")}</p>
                <p>{t("serial.setupSecurity")}</p>
                <p>{t("serial.setupBridge")}</p>
              </div>
            </div>
          </div>
        </div>
      )}
    </>
  );
});
