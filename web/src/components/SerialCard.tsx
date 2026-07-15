import {
  forwardRef,
  useCallback,
  useImperativeHandle,
  useRef,
  useState,
} from "react";
import { Columns2, LayoutPanelTop, ShieldAlert, Terminal as TerminalIcon } from "lucide-react";
import { Card } from "./ui";
import {
  SerialTerminalPane,
  type SerialChannelHandle,
  type SerialChannelId,
  type SerialChannelStatus,
} from "./SerialTerminalPane";
import { useI18n } from "@/lib/i18n";

const CH347_VID = 0x1a86;
const CHANNELS: SerialChannelId[] = ["uart0", "uart1"];

type LayoutMode = "tabs" | "split";

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

export const SerialCard = forwardRef<SerialAutomationHandle, {
  vinRoute?: string;
  onSetVin: (route: "1.8v" | "3.3v") => Promise<void>;
}>(function SerialCard({ vinRoute, onSetVin }, automationRef) {
  const { t } = useI18n();
  const [webSerialSupported] = useState(
    typeof navigator !== "undefined" && "serial" in navigator
  );
  const [activeChannel, setActiveChannel] = useState<SerialChannelId>("uart0");
  const [layout, setLayout] = useState<LayoutMode>("tabs");
  const [changingVoltage, setChangingVoltage] = useState(false);
  const [sharedError, setSharedError] = useState<string | null>(null);
  const [statuses, setStatuses] = useState<Record<SerialChannelId, SerialChannelStatus>>({
    uart0: EMPTY_STATUS,
    uart1: EMPTY_STATUS,
  });

  const activeChannelRef = useRef<SerialChannelId>("uart0");
  const uart0Ref = useRef<SerialChannelHandle>(null);
  const uart1Ref = useRef<SerialChannelHandle>(null);
  const assignedPortsRef = useRef(new Map<SerialChannelId, SerialPort>());

  const channelHandle = (channel: SerialChannelId) =>
    channel === "uart0" ? uart0Ref.current : uart1Ref.current;

  const selectChannel = (channel: SerialChannelId) => {
    activeChannelRef.current = channel;
    setActiveChannel(channel);
  };

  const onStatus = useCallback((channel: SerialChannelId, status: SerialChannelStatus) => {
    setStatuses((current) => current[channel] === status ? current : { ...current, [channel]: status });
  }, []);

  const requestPort = useCallback(async (channel: SerialChannelId) => {
    const assigned = assignedPortsRef.current.get(channel);
    if (assigned) return assigned;

    const ports = (await navigator.serial.getPorts()).filter((candidate) =>
      candidate.getInfo().usbVendorId === CH347_VID
    );
    const portsInUse = new Set(assignedPortsRef.current.values());
    const available = ports.filter((candidate) => !portsInUse.has(candidate));
    const port = available.length === 1
      ? available[0]
      : await navigator.serial.requestPort({ filters: [{ usbVendorId: CH347_VID }] });
    const conflictingChannel = CHANNELS.find((candidate) =>
      candidate !== channel && assignedPortsRef.current.get(candidate) === port
    );
    if (conflictingChannel) {
      throw new Error(t("serial.portInUse").replaceAll("{channel}", conflictingChannel.toUpperCase()));
    }
    assignedPortsRef.current.set(channel, port);
    return port;
  }, [t]);

  const releasePort = useCallback((
    channel: SerialChannelId,
    port: SerialPort,
    physicalDisconnect: boolean
  ) => {
    if (physicalDisconnect && assignedPortsRef.current.get(channel) === port) {
      assignedPortsRef.current.delete(channel);
    }
  }, []);

  useImperativeHandle(automationRef, () => ({
    isConnected: (channel = activeChannelRef.current) =>
      channelHandle(channel)?.isConnected() ?? false,
    connectedChannels: () => CHANNELS.filter((channel) =>
      channelHandle(channel)?.isConnected()
    ),
    clear: (channel = activeChannelRef.current) => channelHandle(channel)?.clear(),
    subscribe: (listener, channel = activeChannelRef.current) =>
      channelHandle(channel)?.subscribe(listener) ?? (() => {}),
  }), []);

  async function changeVin(next: "1.8v" | "3.3v") {
    if (next === vinRoute) return;
    const current = vinRoute === "1.8v" || vinRoute === "3.3v" ? vinRoute : t("serial.vioUnknown");
    const connected = CHANNELS.filter((channel) => statuses[channel].connected);
    const confirmed = window.confirm(
      t("serial.vioConfirm")
        .replaceAll("{current}", current)
        .replaceAll("{next}", next)
        .replaceAll("{channels}", connected.length > 0 ? connected.map((channel) => channel.toUpperCase()).join(" / ") : t("serial.none"))
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

  const connectedCount = CHANNELS.filter((channel) => statuses[channel].connected).length;

  return (
    <Card
      title={t("serial.title")}
      subtitle={t("serial.subtitle")}
      icon={TerminalIcon}
      contentClassName="flex min-h-0 flex-col"
      right={
        <div className="flex max-w-full flex-wrap items-center justify-end gap-2">
          {layout === "tabs" && (
            <div className="inline-flex rounded-xl border border-line/70 bg-panel2 p-1" role="tablist" aria-label={t("serial.channels")}>
              {CHANNELS.map((channel) => {
                const status = statuses[channel];
                return (
                  <button
                    key={channel}
                    type="button"
                    role="tab"
                    aria-selected={activeChannel === channel}
                    onClick={() => selectChannel(channel)}
                    className={`flex min-h-8 items-center gap-2 rounded-lg px-3 text-xs font-semibold transition-colors ${activeChannel === channel ? "bg-panel text-ink shadow-sm" : "text-ink-dim hover:text-ink"}`}
                  >
                    <span className={`h-2 w-2 rounded-full ${status.connected ? "bg-ok shadow-[0_0_7px_rgb(var(--c-ok))]" : status.connecting ? "animate-pulse bg-warn" : "bg-ink-dim/40"}`} />
                    {channel.toUpperCase()}
                    {status.connected && <span className="font-mono text-[9px] font-normal text-ink-dim">RX {status.rxBytes.toLocaleString()}</span>}
                  </button>
                );
              })}
            </div>
          )}
          <div className="inline-flex rounded-xl border border-line/70 bg-panel2 p-1" role="group" aria-label={t("serial.layout")}>
            <button
              type="button"
              onClick={() => setLayout("tabs")}
              aria-pressed={layout === "tabs"}
              title={t("serial.layout.tabs")}
              className={`grid h-8 w-8 place-items-center rounded-lg transition-colors ${layout === "tabs" ? "bg-panel text-brand shadow-sm" : "text-ink-dim hover:text-ink"}`}
            >
              <LayoutPanelTop size={15} />
            </button>
            <button
              type="button"
              onClick={() => setLayout("split")}
              aria-pressed={layout === "split"}
              title={t("serial.layout.split")}
              className={`grid h-8 w-8 place-items-center rounded-lg transition-colors ${layout === "split" ? "bg-panel text-brand shadow-sm" : "text-ink-dim hover:text-ink"}`}
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
              onChange={(event) => void changeVin(event.target.value as "1.8v" | "3.3v")}
              disabled={!vinRoute || changingVoltage}
              className="rounded-md border border-warn/30 bg-panel px-2 py-1 text-xs font-semibold text-ink outline-none focus-visible:ring-2 focus-visible:ring-warn/40 disabled:opacity-50"
            >
              <option value="" disabled>—</option>
              <option value="3.3v">3.3V</option>
              <option value="1.8v">1.8V</option>
            </select>
          </label>
        </div>
      }
    >
      {sharedError && (
        <div className="mb-3 rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
          {sharedError}
        </div>
      )}

      <div className={layout === "split" ? "grid min-h-0 gap-3 lg:grid-cols-2" : "min-h-0"}>
        {CHANNELS.map((channel) => (
          <div
            key={channel}
            className={`min-h-0 min-w-0 ${layout === "tabs" && activeChannel !== channel ? "hidden" : "flex"}`}
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
        {!webSerialSupported && <span>{t("serial.noWebSerial")}</span>}
      </div>
    </Card>
  );
});
