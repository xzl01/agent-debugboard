import { useEffect, useRef, useState } from "react";
import { Copy, Download, Maximize2, Minus, Plus, Settings2, Trash2 } from "lucide-react";
import type { Terminal as XTerm } from "@xterm/xterm";
import type { FitAddon } from "@xterm/addon-fit";
import "@xterm/xterm/css/xterm.css";
import { Button } from "./ui";
import { useI18n } from "@/lib/i18n";
import { useJ4SerialSnapshot } from "@/lib/j4SerialReact";
import { j4ConnectionState, type J4SerialSession } from "@/lib/j4SerialSession";
import { J4_SERIAL_BAUD_RATES, type J4SerialConfig } from "@/lib/j4SerialProtocol";
import { DEFAULT_SERIAL_LINE_ENDING, normalizeSerialTerminalInput, type SerialLineEnding } from "@/lib/serialInput";
import { terminalFontFamilyFromRoot } from "@/lib/terminalFont";
import { resolveTerminalTheme } from "@/lib/terminalTheme";
import { formatBytes } from "@/lib/utils";

const LOG_LIMIT = 1024 * 1024;
const OUTPUT_LIMIT = 64 * 1024;
const iconClass = "grid h-10 w-10 shrink-0 place-items-center rounded-md text-ink-dim transition-colors hover:bg-panel2 hover:text-ink focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40 disabled:opacity-30";
const selectClass = "h-10 min-w-0 rounded-md border border-line/70 bg-panel px-2 text-xs text-ink outline-none focus-visible:ring-2 focus-visible:ring-brand/40 disabled:opacity-50";

export function SerialTerminalPane({ session, fillHeight = false }: {
  session: J4SerialSession;
  fillHeight?: boolean;
}) {
  const { t } = useI18n();
  const snapshot = useJ4SerialSnapshot(session);
  const automationActive = snapshot.automationActive;
  const [fontSize, setFontSize] = useState(13);
  const [lineEnding, setLineEnding] = useState<SerialLineEnding>(DEFAULT_SERIAL_LINE_ENDING);
  const [showConfig, setShowConfig] = useState(false);
  const [draft, setDraft] = useState<J4SerialConfig | null>(null);
  const [logBytes, setLogBytes] = useState(0);
  const [localDropped, setLocalDropped] = useState(0);
  const [terminalVersion, setTerminalVersion] = useState(0);
  const hostRef = useRef<HTMLDivElement>(null);
  const panelRef = useRef<HTMLElement>(null);
  const termRef = useRef<XTerm | null>(null);
  const fitRef = useRef<FitAddon | null>(null);
  const lineEndingRef = useRef(lineEnding);
  const fontSizeRef = useRef(fontSize);
  fontSizeRef.current = fontSize;
  const aliveRef = useRef(true);
  const generationRef = useRef<number | null>(null);
  const renderEpochRef = useRef(0);
  const logRef = useRef<Uint8Array[]>([]);
  const logSizeRef = useRef(0);
  const outputRef = useRef<Uint8Array[]>([]);
  const outputSizeRef = useRef(0);
  const outputBusyRef = useRef(false);
  const renderFrameRef = useRef<number | null>(null);

  const scheduleOutput = () => {
    const term = termRef.current;
    const epoch = renderEpochRef.current;
    if (!term || outputBusyRef.current || renderFrameRef.current !== null) return;
    renderFrameRef.current = requestAnimationFrame(() => {
      if (epoch !== renderEpochRef.current || termRef.current !== term) return;
      renderFrameRef.current = null;
      if (!outputSizeRef.current) return;
      const chunks = outputRef.current;
      const bytes = new Uint8Array(outputSizeRef.current);
      let offset = 0;
      for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.length; }
      outputRef.current = []; outputSizeRef.current = 0;
      outputBusyRef.current = true;
      // Uint8Array lets xterm's streaming UTF-8 decoder span binary WS frames.
      term.write(bytes, () => {
        // A disposed renderer must not unlock or drain its successor's queue.
        if (epoch !== renderEpochRef.current || termRef.current !== term) return;
        outputBusyRef.current = false;
        if (aliveRef.current) scheduleOutput();
      });
    });
  };

  const receive = (bytes: Uint8Array) => {
    const copy = bytes.slice();
    logRef.current.push(copy);
    logSizeRef.current += copy.length;
    while (logSizeRef.current > LOG_LIMIT) {
      const first = logRef.current[0];
      const excess = logSizeRef.current - LOG_LIMIT;
      if (first.length <= excess) { logRef.current.shift(); logSizeRef.current -= first.length; }
      else { logRef.current[0] = first.slice(excess); logSizeRef.current -= excess; }
    }
    setLogBytes(logSizeRef.current);
    if (outputSizeRef.current + copy.length <= OUTPUT_LIMIT) {
      outputRef.current.push(copy);
      outputSizeRef.current += copy.length;
      scheduleOutput();
    } else setLocalDropped((count) => count + copy.length);
  };

  const clear = () => {
    logRef.current = []; logSizeRef.current = 0;
    outputRef.current = []; outputSizeRef.current = 0;
    setLogBytes(0); setLocalDropped(0);
    termRef.current?.clear();
    termRef.current?.write("\u001b[2J\u001b[H");
  };

  useEffect(() => {
    const host = hostRef.current;
    if (!host) return;
    aliveRef.current = true;
    let disposed = false;
    let modules: { xterm: typeof import("@xterm/xterm"); addon: typeof import("@xterm/addon-fit") } | null = null;
    let term: XTerm | null = null;
    let fit: FitAddon | null = null;
    let resize: ResizeObserver | null = null;
    let theme: MutationObserver | null = null;
    const disposables: { dispose(): void }[] = [];
    const disposeRenderer = () => {
      renderEpochRef.current += 1;
      if (renderFrameRef.current !== null) cancelAnimationFrame(renderFrameRef.current);
      renderFrameRef.current = null;
      outputRef.current = []; outputSizeRef.current = 0; outputBusyRef.current = false;
      termRef.current = null; fitRef.current = null;
      disposables.splice(0).forEach((item) => item.dispose());
      resize?.disconnect(); theme?.disconnect();
      fit?.dispose(); term?.dispose();
      fit = null; term = null;
      host.replaceChildren();
    };
    const createRenderer = () => {
      if (disposed || !modules) return;
      const epoch = renderEpochRef.current;
      const current = session.getState();
      const canInput = j4ConnectionState(current) === "ready" && !current.backpressure && !current.automationActive;
      const nextTerm = new modules.xterm.Terminal({ cursorBlink: canInput, disableStdin: !canInput, screenReaderMode: true,
        fontFamily: terminalFontFamilyFromRoot(), fontSize: fontSizeRef.current, lineHeight: 1.15,
        scrollback: 10000, theme: resolveTerminalTheme() });
      const nextFit = new modules.addon.FitAddon(); nextTerm.loadAddon(nextFit); nextTerm.open(host);
      term = nextTerm; fit = nextFit; termRef.current = nextTerm; fitRef.current = nextFit;
      const active = () => !disposed && epoch === renderEpochRef.current && termRef.current === nextTerm;
      const fitTerminal = () => { if (active()) try { nextFit.fit(); } catch { /* Transient hidden layout. */ } };
      document.addEventListener("fullscreenchange", fitTerminal);
      disposables.push({ dispose: () => document.removeEventListener("fullscreenchange", fitTerminal) });
      setTerminalVersion((version) => version + 1);
      const send = (bytes: string | Uint8Array) => {
        if (!active()) return;
        void (typeof bytes === "string" ? session.writeManual(bytes) : session.writeBinaryManual(bytes)).catch(() => {});
      };
      disposables.push(nextTerm.onData((data) => send(normalizeSerialTerminalInput(data, lineEndingRef.current))));
      disposables.push(nextTerm.onBinary((data) => send(Uint8Array.from(data, (char) => char.charCodeAt(0) & 0xff))));
      resize = new ResizeObserver(fitTerminal); resize.observe(host);
      theme = new MutationObserver(() => {
        if (!active()) return;
        nextTerm.options.theme = resolveTerminalTheme();
        nextTerm.options.fontFamily = terminalFontFamilyFromRoot();
        nextTerm.refresh(0, Math.max(0, nextTerm.rows - 1)); fitTerminal();
      });
      theme.observe(document.documentElement, { attributes: true,
        attributeFilter: ["data-theme", "data-terminal-theme", "data-terminal-font-family"] });
      fitTerminal(); scheduleOutput();
    };
    generationRef.current = session.getState().serial?.generation ?? null;
    const unsubscribeState = session.subscribeState(() => {
      const generation = session.getState().serial?.generation ?? null;
      if (generation === generationRef.current) return;
      generationRef.current = generation;
      // reset() leaves xterm's parser/write buffer alive. Destroy it before
      // the next generation can synchronously deliver its first UART bytes.
      disposeRenderer();
      createRenderer();
    });
    const unsubscribeBytes = session.subscribeBytes(receive);
    const unsubscribeClear = session.subscribeClear(clear);
    void Promise.all([import("@xterm/xterm"), import("@xterm/addon-fit")]).then(([xterm, addon]) => {
      if (disposed) return;
      modules = { xterm, addon };
      createRenderer();
    });
    return () => {
      disposed = true; aliveRef.current = false;
      unsubscribeState(); unsubscribeBytes(); unsubscribeClear();
      disposeRenderer();
    };
  }, [session]);

  const connected = snapshot.phase === "connected";
  const connecting = snapshot.phase === "connecting";
  const readOnly = !connected || snapshot.serial?.state !== "ready" || snapshot.serial.read_only
    || snapshot.configuring || snapshot.backpressure || automationActive;
  useEffect(() => { lineEndingRef.current = lineEnding; }, [lineEnding]);
  useEffect(() => {
    const term = termRef.current;
    if (!term) return;
    term.options.disableStdin = readOnly;
    term.options.cursorBlink = !readOnly;
  }, [readOnly, terminalVersion]);
  useEffect(() => {
    if (!termRef.current) return;
    termRef.current.options.fontSize = fontSize;
    try { fitRef.current?.fit(); } catch { /* Fullscreen transitions can hide the host. */ }
  }, [fontSize, terminalVersion]);
  useEffect(() => { if (snapshot.serial) setDraft({ ...snapshot.serial.config }); }, [snapshot.serial]);

  const statusKey = snapshot.phase === "unavailable" ? "serial.unavailable"
    : connecting ? "serial.connecting" : !connected ? "serial.disconnected"
    : snapshot.serial?.state === "faulted" ? "serial.faulted"
    : snapshot.serial?.state === "paused" ? "serial.suspended"
    : snapshot.configuring || snapshot.serial?.state === "reconfig" ? "serial.configuring"
    : snapshot.serial?.read_only ? "serial.readOnly" : "serial.connected";
  const errorText = snapshot.phase === "unavailable" ? t("serial.unavailableHint")
    : snapshot.error?.code === "backpressure" || snapshot.error?.code === "queue_full" ? t("serial.inputRejected")
    : snapshot.error?.code === "generation_conflict" || snapshot.error?.code === "config_conflict"
      || snapshot.error?.code === "cdc_active" || snapshot.error?.code === "dtr_active"
      || snapshot.error?.code === "configuration_busy"
      ? t("serial.configConflict")
      : snapshot.error?.code === "connection_closed" || snapshot.error?.code === "connection_error" || snapshot.error?.code === "connect_timeout"
        ? t("serial.connectionLost")
        : snapshot.error?.code === "generation_changed" ? t("serial.generationChanged")
          : snapshot.error?.code === "tx_timeout" || snapshot.error?.code === "tx_canceled" ? t("serial.writeIncomplete")
            : snapshot.error?.code === "config_timeout" || snapshot.error?.code === "config_failed" ? t("serial.configFailed")
              : snapshot.error?.code === "serial_not_ready" ? t("serial.readOnlyHint") : snapshot.error?.message;
  const inputHint = automationActive ? "serial.automationInputLocked" : snapshot.backpressure ? "serial.backpressure"
    : !connected ? "serial.connectToInput" : readOnly ? "serial.readOnlyHint" : "serial.directInput";

  async function downloadLog() {
    if (!logSizeRef.current) return;
    const bytes = new Uint8Array(logSizeRef.current);
    let offset = 0;
    for (const chunk of logRef.current) { bytes.set(chunk, offset); offset += chunk.length; }
    const url = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = `radxa-linkr-j4-${new Date().toISOString().replaceAll(":", "-")}.bin`;
    anchor.hidden = true;
    document.body.appendChild(anchor); anchor.click(); anchor.remove();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  return (
    <section ref={panelRef} data-testid="serial-terminal-j4" aria-label={`J4 ${t("serial.console")}`}
      className="flex min-h-0 min-w-0 flex-1 flex-col overflow-hidden rounded-xl border border-line/80 bg-terminal shadow-inner fullscreen:h-screen fullscreen:w-screen">
      <div className="flex flex-wrap items-center justify-between gap-2 border-b border-line/70 px-3 py-2 text-terminal-ink">
        <div className="flex min-w-0 flex-wrap items-center gap-x-2 gap-y-1">
          <span aria-hidden="true" className={`h-2 w-2 shrink-0 rounded-full ${connected && !readOnly ? "bg-ok" : connecting ? "animate-pulse bg-warn motion-reduce:animate-none" : "bg-ink-dim/50"}`} />
          <span className="font-mono text-xs font-bold">J4</span>
          <span className="rounded border border-line/70 bg-panel2/35 px-1.5 py-0.5 font-mono text-[10px] text-ink-dim" data-testid="terminal-engine">xterm.js · MCU UART</span>
          <span role="status" className="text-xs text-ink-dim">{t(statusKey)}</span>
          {snapshot.serial && <span className="font-mono text-[10px] text-ink-dim">{snapshot.serial.config.baud.toLocaleString()} · {snapshot.serial.config.data_bits}{snapshot.serial.config.parity[0].toUpperCase()}{snapshot.serial.config.stop_bits}</span>}
        </div>
        <div className="flex flex-wrap items-center gap-1">
          <Button type="button" variant={connected ? "default" : "primary"} className="min-h-10 text-xs" disabled={automationActive}
            onClick={() => { void (connected || connecting ? session.disconnect() : session.connect()); }}>
            {connected || connecting ? t("serial.disconnect") : t("serial.connectJ4")}
          </Button>
          <button type="button" className={iconClass} aria-label={`J4 ${t("serial.configuration")}`} title={t("serial.configuration")}
            aria-expanded={showConfig} onClick={() => setShowConfig((value) => !value)}><Settings2 size={15} /></button>
          <select value={lineEnding} onChange={(event) => setLineEnding(event.target.value as SerialLineEnding)}
            aria-label={`J4 ${t("serial.lineEnding")}`} className={selectClass}>
            <option value="cr">CR</option><option value="crlf">CRLF</option><option value="lf">LF</option>
          </select>
          <button type="button" className={iconClass} aria-label={`J4 ${t("serial.fontDown")}`} title={t("serial.fontDown")} disabled={fontSize <= 10} onClick={() => setFontSize((size) => size - 1)}><Minus size={14} /></button>
          <button type="button" className={iconClass} aria-label={`J4 ${t("serial.fontUp")}`} title={t("serial.fontUp")} disabled={fontSize >= 20} onClick={() => setFontSize((size) => size + 1)}><Plus size={14} /></button>
          <button type="button" className={iconClass} aria-label={`J4 ${t("serial.copy")}`} title={t("serial.copy")} onClick={() => { const text = termRef.current?.getSelection(); if (text) void navigator.clipboard?.writeText(text).catch(() => {}); }}><Copy size={14} /></button>
          <button type="button" className={iconClass} aria-label={`J4 ${t("serial.downloadLog")}`} title={t("serial.downloadLog")} disabled={!logBytes} onClick={() => void downloadLog()}><Download size={14} /></button>
          <button type="button" className={iconClass} aria-label={`J4 ${t("serial.clear")}`} title={t("serial.clear")} onClick={clear}><Trash2 size={14} /></button>
          <button type="button" className={iconClass} aria-label={`J4 ${t("serial.fullscreen")}`} title={t("serial.fullscreen")} onClick={() => {
            void (document.fullscreenElement === panelRef.current ? document.exitFullscreen() : panelRef.current?.requestFullscreen())?.catch(() => {});
          }}><Maximize2 size={14} /></button>
        </div>
      </div>

      {showConfig && <form className="space-y-3 border-b border-line/70 bg-panel2/40 p-3 text-xs" onSubmit={(event) => {
        event.preventDefault(); if (draft) void session.configure(draft).catch(() => {});
      }}>
        <div className="flex flex-wrap items-end gap-3">
          <label className="flex flex-col gap-1 text-ink-dim">{t("serial.baud")}<select className={selectClass} value={draft?.baud ?? ""} disabled={!draft || snapshot.configuring} onChange={(event) => setDraft((value) => value && ({ ...value, baud: Number(event.target.value) }))}>
            {!draft && <option value="">—</option>}{J4_SERIAL_BAUD_RATES.map((baud) => <option key={baud} value={baud}>{baud.toLocaleString()}</option>)}
          </select></label>
          <label className="flex flex-col gap-1 text-ink-dim">{t("serial.dataBits")}<select className={selectClass} value={draft?.data_bits ?? 8} disabled={!draft || snapshot.configuring} onChange={(event) => setDraft((value) => value && ({ ...value, data_bits: Number(event.target.value) as J4SerialConfig["data_bits"] }))}>
            {[5, 6, 7, 8].map((bits) => <option key={bits} value={bits}>{bits}</option>)}
          </select></label>
          <label className="flex flex-col gap-1 text-ink-dim">{t("serial.parity")}<select className={selectClass} value={draft?.parity ?? "none"} disabled={!draft || snapshot.configuring} onChange={(event) => setDraft((value) => value && ({ ...value, parity: event.target.value as J4SerialConfig["parity"] }))}>
            <option value="none">{t("serial.parity.none")}</option><option value="odd">{t("serial.parity.odd")}</option><option value="even">{t("serial.parity.even")}</option>
          </select></label>
          <label className="flex flex-col gap-1 text-ink-dim">{t("serial.stopBits")}<select className={selectClass} value={draft?.stop_bits ?? 1} disabled={!draft || snapshot.configuring} onChange={(event) => setDraft((value) => value && ({ ...value, stop_bits: Number(event.target.value) as 1 | 2 }))}>
            <option value="1">1</option><option value="2">2</option>
          </select></label>
          <label className="flex flex-col gap-1 text-ink-dim">{t("serial.flowControl")}<select className={selectClass} value={draft?.flow_control ?? "none"} disabled={!draft || snapshot.configuring} onChange={(event) => setDraft((value) => value && ({ ...value, flow_control: event.target.value as J4SerialConfig["flow_control"] }))}>
            <option value="none">{t("serial.none")}</option>
          </select></label>
          <Button type="submit" variant="default" className="min-h-10 text-xs" disabled={!connected || !draft || snapshot.configuring || snapshot.pendingWrites > 0 || automationActive || snapshot.serial?.read_only || snapshot.serial?.state === "faulted"}>
            {t(snapshot.configuring ? "serial.configuring" : "serial.applyConfig")}
          </Button>
        </div>
        <p className="text-ink-dim">{t("serial.sharedConfig")}{snapshot.serial ? ` · ${t("serial.generation")} ${snapshot.serial.generation}` : ""}</p>
        {snapshot.serial?.dtr && <p role="status" className="text-warn">{t("serial.dtrActive")}</p>}
      </form>}
      {errorText && <div role="alert" className={`break-words border-b px-3 py-2 text-xs ${snapshot.phase === "unavailable" || snapshot.error?.code === "backpressure" ? "border-warn/30 bg-warn/10 text-warn" : "border-danger/30 bg-danger/10 text-danger"}`}>{errorText}</div>}
      {(snapshot.droppedBytes > 0 || localDropped > 0) && <p role="status" className="border-b border-warn/30 bg-warn/10 px-3 py-2 text-xs text-warn">{t("serial.dropped").replaceAll("{bytes}", formatBytes(snapshot.droppedBytes + localDropped))}</p>}
      <div ref={hostRef} role="application" aria-label={`J4 ${t("serial.terminalAria")}`} tabIndex={0} onClick={() => termRef.current?.focus()}
        className={`${fillHeight ? "min-h-[360px] flex-1" : "h-[clamp(400px,62vh,760px)] shrink-0"} terminal-host min-w-0 overflow-hidden bg-terminal p-2 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-inset focus-visible:ring-brand/40 [&_.xterm]:h-full [&_.xterm]:max-w-full`} />
      <footer className="flex min-h-10 flex-wrap items-center gap-x-4 gap-y-1 border-t border-line/70 px-3 py-2 text-[10px] text-ink-dim">
        <span aria-live="polite">{t(inputHint)}</span>
        <span className="font-mono" title={t("serial.logCacheHint")}>{t("serial.logCached")} {formatBytes(logBytes)}</span>
        <span className="ml-auto font-mono">RX {snapshot.rxBytes.toLocaleString()}</span><span className="font-mono">TX {snapshot.txBytes.toLocaleString()}</span>
        <button type="button" disabled={readOnly} onClick={() => { void session.writeManual("\u0003").catch(() => {}); }} className="min-h-8 rounded border border-line/70 px-2 font-mono disabled:opacity-30">Ctrl-C</button>
      </footer>
    </section>
  );
}
