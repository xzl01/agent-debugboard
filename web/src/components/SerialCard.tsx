import { useEffect, useRef, useState } from "react";
import { Terminal, Usb, Plug, Trash2, Send } from "lucide-react";
import { Button, Card } from "./ui";
import { useI18n } from "@/lib/i18n";

const BAUD_RATES = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1500000];
const CH347_VID = 0x1a86;
const BRIDGE_PORT = 8787;
const MAX_LOG = 60000;

type Source = "webserial" | "bridge" | null;

export function SerialCard() {
  const { t } = useI18n();
  const [webSerialSupported] = useState(
    typeof navigator !== "undefined" && "serial" in navigator
  );
  const [source, setSource] = useState<Source>(null);
  const [baud, setBaud] = useState(115200);
  const [portInfo, setPortInfo] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [log, setLog] = useState("");
  const [input, setInput] = useState("");

  const portRef = useRef<SerialPort | null>(null);
  const readerRef = useRef<ReadableStreamDefaultReader<Uint8Array> | null>(null);
  const wsRef = useRef<WebSocket | null>(null);
  const logRef = useRef<HTMLPreElement | null>(null);

  const append = (text: string) => {
    setLog((prev) => {
      const next = prev + text;
      return next.length > MAX_LOG ? next.slice(next.length - MAX_LOG) : next;
    });
  };

  useEffect(() => {
    if (logRef.current) logRef.current.scrollTop = logRef.current.scrollHeight;
  }, [log]);

  useEffect(() => () => disconnect(), []); // eslint-disable-line react-hooks/exhaustive-deps

  function disconnect() {
    try {
      readerRef.current?.cancel();
    } catch {
      /* ignore */
    }
    try {
      portRef.current?.close();
    } catch {
      /* ignore */
    }
    portRef.current = null;
    try {
      wsRef.current?.close();
    } catch {
      /* ignore */
    }
    wsRef.current = null;
    setSource(null);
  }

  async function connectWebSerial() {
    setError(null);
    try {
      const port = await navigator.serial.requestPort({
        filters: [{ usbVendorId: CH347_VID }],
      });
      await port.open({ baudRate: baud });
      portRef.current = port;
      setSource("webserial");
      setPortInfo(`CH347F @ ${baud} baud (Web Serial)`);
      const decoder = new TextDecoder();
      const reader = port.readable!.getReader();
      readerRef.current = reader;
      (async () => {
        try {
          while (true) {
            const { value, done } = await reader.read();
            if (done) break;
            if (value) append(decoder.decode(value));
          }
        } catch (e) {
          append(`\r\n[read error: ${e instanceof Error ? e.message : e}]\r\n`);
        }
      })();
      port.addEventListener("disconnect", () => {
        append("\r\n[device disconnected]\r\n");
        disconnect();
      });
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  function connectBridge() {
    setError(null);
    const url = `ws://127.0.0.1:${BRIDGE_PORT}/serial`;
    const ws = new WebSocket(url);
    wsRef.current = ws;
    ws.onopen = () => ws.send(JSON.stringify({ type: "open", baud }));
    ws.onmessage = (ev) => {
      try {
        const msg = JSON.parse(ev.data);
        if (msg.type === "data") append(msg.text);
        else if (msg.type === "opened") {
          setSource("bridge");
          setPortInfo(`${msg.path} @ ${msg.baud} baud (bridge)`);
        } else if (msg.type === "error") setError(msg.message);
        else if (msg.type === "closed") {
          append("\r\n[bridge closed]\r\n");
          disconnect();
        }
      } catch {
        /* ignore malformed frames */
      }
    };
    ws.onerror = () => setError(t("serial.bridgeError"));
  }

  function send() {
    if (!input) return;
    const text = input + "\r\n";
    if (source === "webserial" && portRef.current?.writable) {
      const writer = portRef.current.writable.getWriter();
      writer
        .write(new TextEncoder().encode(text))
        .then(() => writer.releaseLock())
        .catch(() => {});
    } else if (source === "bridge" && wsRef.current) {
      wsRef.current.send(text);
    }
    append(`> ${text}`);
    setInput("");
  }

  return (
    <Card
      title={t("serial.title")}
      subtitle={t("serial.subtitle")}
      icon={Terminal}
      right={
        source ? (
          <Button variant="ghost" onClick={disconnect}>
            {t("serial.disconnect")}
          </Button>
        ) : (
          <div className="flex max-w-full flex-wrap gap-2">
            {webSerialSupported && (
              <Button variant="primary" onClick={connectWebSerial}>
                <Usb size={16} /> {t("serial.webSerial")}
              </Button>
            )}
            <Button variant="default" onClick={connectBridge}>
              <Plug size={16} /> {t("serial.bridge")}
            </Button>
          </div>
        )
      }
    >
      {!source && (
        <div className="mb-3 text-xs text-ink-dim">
          {t("serial.connect")}
          {webSerialSupported ? t("serial.webSerialHint") : t("serial.noWebSerial")}
        </div>
      )}

      {error && (
        <div className="mb-2 rounded-md border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
          {error}
        </div>
      )}

      <pre
        ref={logRef}
        className="h-[clamp(360px,56vh,720px)] overflow-auto whitespace-pre-wrap break-words rounded-xl border border-line/80 bg-terminal p-3 font-mono text-xs text-terminal-ink shadow-inner"
      >
        {log || (portInfo ? "" : t("serial.noConnection"))}
      </pre>

      <div className="mt-3 grid grid-cols-[auto_minmax(0,1fr)_auto_auto] items-center gap-2 max-sm:grid-cols-[auto_minmax(0,1fr)_auto]">
        <select
          value={baud}
          onChange={(e) => setBaud(Number(e.target.value))}
          disabled={!!source}
          className="rounded-md border border-line/70 bg-panel2 px-2 py-1.5 text-xs text-ink outline-none"
        >
          {BAUD_RATES.map((b) => (
            <option key={b} value={b}>
              {b}
            </option>
          ))}
        </select>
        <input
          value={input}
          onChange={(e) => setInput(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter") send();
          }}
          placeholder={t("serial.placeholder")}
          className="flex-1 rounded-md border border-line/70 bg-panel2 px-3 py-1.5 text-sm text-ink outline-none placeholder:text-ink-dim/80"
        />
        <Button variant="default" onClick={send} disabled={!source}>
          <Send size={16} />
        </Button>
        <Button variant="ghost" className="max-sm:col-start-3" onClick={() => setLog("")}>
          <Trash2 size={16} />
        </Button>
      </div>
    </Card>
  );
}
