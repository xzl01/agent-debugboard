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
  Archive,
  Check,
  Columns2,
  Copy,
  LayoutPanelTop,
  Plug,
  ShieldAlert,
  Usb,
  X,
} from "lucide-react";
import { Button } from "./ui";
import {
  isSerialDisconnectBlocked,
  SerialTerminalPane,
  type SerialChannelHandle,
  type SerialChannelId,
  type SerialChannelStatus,
} from "./SerialTerminalPane";
import { useI18n } from "@/lib/i18n";
import { HostSerialLogs } from "./HostSerialLogs";

const CH347_VID = 0x1a86;
const CHANNELS: SerialChannelId[] = ["uart0", "uart1"];
const BOARD_HTTP_ORIGIN = "http://172.29.203.1";
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
type ContextDrawerTab = "connection" | "archive";
type CopyState = "idle" | "success" | "error";
type CopyTarget = "flag" | "origin";

const EMPTY_STATUS: SerialChannelStatus = {
  connected: false,
  connecting: false,
  automationActive: false,
  brokerWriteLocked: false,
  source: null,
  portInfo: "",
  baud: 115200,
  lineEnding: "cr",
  rxBytes: 0,
  txBytes: 0,
};

export type { SerialChannelId } from "./SerialTerminalPane";

export interface SerialConnectionSummary {
  readonly uart0: boolean;
  readonly uart1: boolean;
  readonly bridgeActive: boolean;
}

export interface SerialAutomationHandle {
  isConnected: (channel?: SerialChannelId) => boolean;
  connectedChannels: () => SerialChannelId[];
  clear: (channel?: SerialChannelId) => void;
  write: (data: string, channel?: SerialChannelId) => Promise<void>;
  setAutomationActive: (active: boolean, channel?: SerialChannelId) => void;
  subscribe: (
    listener: (text: string, receivedAtMs: number) => void,
    channel?: SerialChannelId
  ) => () => void;
}

// allow: SIZE_OK — one dual-UART workspace state machine owns both persistent sessions.
export const SerialCard = forwardRef<
  SerialAutomationHandle,
  {
    vinRoute?: string;
    onSetVin: (route: "1.8v" | "3.3v") => Promise<void>;
    onConnectionChange?: (connections: SerialConnectionSummary) => void;
  }
>(function SerialCard({ vinRoute, onSetVin, onConnectionChange }, automationRef) {
  const { t } = useI18n();
  const isSecureContext = typeof window !== "undefined" && window.isSecureContext;
  const hasWebSerialApi = typeof navigator !== "undefined" && "serial" in navigator;
  const webSerialSupported = isSecureContext && hasWebSerialApi;

  const [activeChannel, setActiveChannel] = useState<SerialChannelId>("uart0");
  const [layout, setLayout] = useState<LayoutMode>("tabs");
  const [changingVoltage, setChangingVoltage] = useState(false);
  const [sharedError, setSharedError] = useState<string | null>(null);
  const [showSetupModal, setShowSetupModal] = useState(false);
  const [showContextDrawer, setShowContextDrawer] = useState(false);
  const [contextDrawerTab, setContextDrawerTab] = useState<ContextDrawerTab>("connection");
  const [copyState, setCopyState] = useState<Record<CopyTarget, CopyState>>({
    flag: "idle",
    origin: "idle",
  });
  const [statuses, setStatuses] = useState<Record<SerialChannelId, SerialChannelStatus>>({
    uart0: EMPTY_STATUS,
    uart1: EMPTY_STATUS,
  });
  const statusesRef = useRef(statuses);

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
    const current = statusesRef.current;
    if (current[channel] === status) return;
    const next = { ...current, [channel]: status };
    statusesRef.current = next;
    setStatuses(next);
    onConnectionChange?.({
      uart0: next.uart0.connected,
      uart1: next.uart1.connected,
      bridgeActive:
        (next.uart0.connected && next.uart0.source === "bridge")
        || (next.uart1.connected && next.uart1.source === "bridge"),
    });
  }, [onConnectionChange]);

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
      write: (data, channel = activeChannelRef.current) => {
        const handle = channelHandle(channel);
        if (!handle) return Promise.reject(new Error(`${channel.toUpperCase()} is unavailable`));
        return handle.write(data);
      },
      setAutomationActive: (active, channel = activeChannelRef.current) =>
        channelHandle(channel)?.setAutomationActive(active),
      subscribe: (listener, channel = activeChannelRef.current) =>
        channelHandle(channel)?.subscribe(listener) ?? (() => {}),
    }),
    []
  );

  useEffect(() => {
    setCopyState({ flag: "idle", origin: "idle" });
  }, [showSetupModal]);

  useEffect(() => {
    if (!showContextDrawer || typeof window === "undefined") return;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") setShowContextDrawer(false);
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [showContextDrawer]);

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
    if (
      CHANNELS.some(
        (channel) =>
          statuses[channel].automationActive ||
          Boolean(channelHandle(channel)?.isAutomationActive())
      )
    ) {
      setSharedError(t("serial.vioAutomationLocked"));
      return;
    }

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

  async function connectChannel(
    channel: SerialChannelId,
    source: "webserial" | "bridge"
  ) {
    const handle = channelHandle(channel);
    if (!handle || statuses[channel].connected || statuses[channel].connecting) return;
    selectChannel(channel);
    setSharedError(null);
    try {
      if (source === "webserial") {
        if (!webSerialSupported) {
          setShowContextDrawer(false);
          setShowSetupModal(true);
          return;
        }
        await handle.connectWebSerial();
      } else {
        handle.connectBridge();
      }
    } catch (reason) {
      setSharedError(reason instanceof Error ? reason.message : String(reason));
    }
  }

  async function disconnectChannel(channel: SerialChannelId) {
    const status = statuses[channel];
    if (!status.connected || isSerialDisconnectBlocked(status)) return;
    setSharedError(null);
    try {
      await channelHandle(channel)?.disconnect();
    } catch (reason) {
      setSharedError(reason instanceof Error ? reason.message : String(reason));
    }
  }

  const connectedCount = CHANNELS.filter((channel) => statuses[channel].connected).length;
  const anyAutomationActive = CHANNELS.some(
    (channel) => statuses[channel].automationActive
  );

  return (
    <>
      <section
        data-testid="serial-workspace"
        className="flex min-h-0 flex-col gap-4 xl:h-[832px]"
      >
        <div
          role="toolbar"
          aria-label={t("serial.title")}
          className="flex min-h-[72px] max-w-full flex-wrap items-center justify-between gap-2 rounded-2xl border border-line/80 bg-panel px-3 py-2"
        >
          <div className="flex max-w-full flex-wrap items-center gap-2">
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
                          ? "bg-panel text-ink ring-1 ring-inset ring-line/70"
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
                    ? "bg-brand/10 text-brand ring-1 ring-inset ring-brand/15"
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
                    ? "bg-brand/10 text-brand ring-1 ring-inset ring-brand/15"
                    : "text-ink-dim hover:text-ink"
                }`}
              >
                <Columns2 size={15} />
              </button>
            </div>
          </div>
          <div className="flex flex-wrap items-center justify-end gap-2">
            <Button
              type="button"
              variant="default"
              className="min-h-10 rounded-xl px-3 text-xs"
              aria-label={`${t("serial.channels")} / ${t("serial.hostLogs.title")}`}
              onClick={() => {
                setContextDrawerTab("connection");
                setShowContextDrawer(true);
              }}
            >
              <Archive size={15} aria-hidden="true" />
              {t("serial.connectedCount").replaceAll("{count}", String(connectedCount))}
            </Button>
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
                disabled={!vinRoute || changingVoltage || anyAutomationActive}
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
        </div>

        {sharedError && (
          <div className="mb-3 rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
            {sharedError}
          </div>
        )}

        <div className={layout === "split" ? "grid min-h-0 flex-1 gap-3 lg:grid-cols-2" : "flex min-h-0 flex-1"}>
          {CHANNELS.map((channel) => (
            <div
              key={channel}
              className={`min-h-0 min-w-0 ${
                layout === "tabs" && activeChannel !== channel ? "hidden" : "flex flex-1"
              }`}
              role={layout === "tabs" ? "tabpanel" : undefined}
            >
              <SerialTerminalPane
                ref={channel === "uart0" ? uart0Ref : uart1Ref}
                channel={channel}
                visible={layout === "split" || activeChannel === channel}
                compact={layout === "split"}
                fillHeight
                webSerialSupported={webSerialSupported}
                requestPort={requestPort}
                releasePort={releasePort}
                onStatus={onStatus}
                onOpenWebSerialSetup={openSetupModal}
              />
            </div>
          ))}
        </div>

      </section>

      {showContextDrawer && (
        <div className="fixed inset-0 z-40" data-testid="serial-context-drawer">
          <button
            type="button"
            className="absolute inset-0 cursor-default bg-terminal/30"
            aria-label={t("serial.setupClose")}
            onClick={() => setShowContextDrawer(false)}
          />
          <aside
            role="dialog"
            aria-modal="true"
            aria-label={`${t("serial.channels")} / ${t("serial.hostLogs.title")}`}
            className="absolute inset-y-0 right-0 flex w-full max-w-[460px] flex-col border-l border-line/70 bg-panel shadow-2xl"
          >
            <header className="flex items-start justify-between gap-3 border-b border-line/60 px-4 py-3">
              <div className="min-w-0">
                <h3 className="text-sm font-semibold text-ink">
                  {t("serial.channels")} / {t("serial.hostLogs.title")}
                </h3>
                <p className="mt-1 text-xs text-ink-dim">{t("serial.dualHint")}</p>
              </div>
              <Button
                type="button"
                variant="ghost"
                className="h-9 min-h-9 w-9 shrink-0 px-0"
                aria-label={t("serial.setupClose")}
                onClick={() => setShowContextDrawer(false)}
              >
                <X size={16} />
              </Button>
            </header>

            <div className="grid grid-cols-2 border-b border-line/60 px-4 pt-2" role="tablist">
              {([
                ["connection", t("serial.channels")],
                ["archive", t("serial.hostLogs.title")],
              ] as const).map(([tab, label]) => (
                <button
                  key={tab}
                  type="button"
                  role="tab"
                  aria-selected={contextDrawerTab === tab}
                  className={`border-b-2 px-3 py-2.5 text-xs font-semibold transition-colors ${
                    contextDrawerTab === tab
                      ? "border-brand text-brand"
                      : "border-transparent text-ink-dim hover:text-ink"
                  }`}
                  onClick={() => setContextDrawerTab(tab)}
                >
                  {label}
                </button>
              ))}
            </div>

            <div className="min-h-0 flex-1 overflow-y-auto p-4">
              {contextDrawerTab === "archive" ? (
                <HostSerialLogs />
              ) : (
                <div className="space-y-4">
                  <p className="text-xs leading-5 text-ink-dim">{t("serial.connect")}</p>
                  <div className="space-y-2">
                    {CHANNELS.map((channel) => {
                      const status = statuses[channel];
                      const connectDisabled = status.connecting || status.automationActive;
                      const disconnectDisabled = isSerialDisconnectBlocked(status);
                      return (
                        <section
                          key={channel}
                          data-testid={`serial-session-${channel}`}
                          className="rounded-xl border border-line/70 bg-panel2/35 p-3"
                        >
                          <div className="flex items-start justify-between gap-3">
                            <div className="min-w-0">
                              <div className="flex items-center gap-2">
                                <span className={`h-2 w-2 rounded-full ${status.connected ? "bg-ok" : status.connecting ? "animate-pulse bg-warn" : "bg-ink-dim/35"}`} />
                                <h4 className="text-xs font-semibold text-ink">{channel.toUpperCase()}</h4>
                                <span className="text-[11px] text-ink-dim">
                                  {status.connecting
                                    ? t("serial.connecting")
                                    : status.connected
                                      ? t("serial.connected")
                                      : t("serial.disconnected")}
                                </span>
                              </div>
                              <p className="mt-1 truncate font-mono text-[10px] text-ink-dim">
                                {status.source === "webserial"
                                  ? t("serial.webSerial")
                                  : status.source === "bridge"
                                    ? t("serial.bridge")
                                    : t("serial.noConnection")}
                                {status.portInfo ? ` · ${status.portInfo}` : ""}
                              </p>
                            </div>
                            <span className="shrink-0 font-mono text-[10px] text-ink-dim">
                              {status.baud.toLocaleString()} · {status.lineEnding.toUpperCase()}
                            </span>
                          </div>

                          <div className="mt-3 flex items-center justify-end gap-2 border-t border-line/50 pt-3">
                            {status.connected || status.connecting ? (
                              <Button
                                type="button"
                                variant="default"
                                className="min-h-8 rounded-lg px-3 py-1 text-xs"
                                disabled={!status.connected || disconnectDisabled}
                                onClick={() => void disconnectChannel(channel)}
                              >
                                {status.connecting ? t("serial.connecting") : t("serial.disconnect")}
                              </Button>
                            ) : (
                              <>
                                <Button
                                  type="button"
                                  variant="primary"
                                  className="min-h-8 rounded-lg px-3 py-1 text-xs"
                                  disabled={connectDisabled}
                                  onClick={() => void connectChannel(channel, "webserial")}
                                >
                                  <Usb size={13} /> {t("serial.webSerial")}
                                </Button>
                                <Button
                                  type="button"
                                  variant="default"
                                  className="min-h-8 rounded-lg px-3 py-1 text-xs"
                                  disabled={connectDisabled}
                                  onClick={() => void connectChannel(channel, "bridge")}
                                >
                                  <Plug size={13} /> {t("serial.bridge")}
                                </Button>
                              </>
                            )}
                          </div>
                        </section>
                      );
                    })}
                  </div>

                  <section className="rounded-xl border border-warn/30 bg-warn/5 p-3">
                    <div className="flex items-center gap-2 text-xs font-semibold text-ink">
                      <ShieldAlert size={14} className="text-warn" />
                      {t("serial.vioLabel")}
                    </div>
                    <div className="mt-3 grid grid-cols-2 gap-2">
                      {(["1.8v", "3.3v"] as const).map((route) => (
                        <Button
                          key={route}
                          type="button"
                          variant={vinRoute === route ? "primary" : "default"}
                          className="min-h-9 rounded-lg px-3 py-1 text-xs"
                          disabled={!vinRoute || changingVoltage || anyAutomationActive}
                          onClick={() => void changeVin(route)}
                        >
                          {route.toUpperCase()}
                        </Button>
                      ))}
                    </div>
                    <p className="mt-2 text-[11px] leading-4 text-warn">{t("serial.vioWarning")}</p>
                  </section>
                </div>
              )}
            </div>
          </aside>
        </div>
      )}

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
