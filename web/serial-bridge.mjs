// Host-side fallback bridge for the Radxa Linkr Debugger web UI.
//
// The onboard CH347F exposes the target board's UART as a USB-serial device on
// the host (WCH VID 1a86). A browser cannot always claim it via Web Serial when
// the OS WCH driver owns the port, so this small Node process opens the port and
// streams it to the web UI over WebSocket. The web UI tries Web Serial first and
// falls back to ws://<host>:8787 through this bridge.
//
//   npm run serial-bridge
//
import { SerialPort } from "serialport";
import { WebSocketServer } from "ws";

const BRIDGE_PORT = 8787;
const CH347_VID = "1a86";

const wss = new WebSocketServer({ host: "0.0.0.0", port: BRIDGE_PORT });

function send(ws, obj) {
  if (ws.readyState === ws.OPEN) ws.send(JSON.stringify(obj));
}

function openSerial(ws, baud) {
  SerialPort.list()
    .then((ports) => {
      const target = ports.find((p) => (p.vendorId || "").toLowerCase() === CH347_VID);
      if (!target) {
        send(ws, { type: "error", message: "No CH347F (VID 1a86) serial port found" });
        return;
      }

      const serial = new SerialPort({ path: target.path, baudRate: baud });
      serial.on("data", (chunk) => send(ws, { type: "data", text: chunk.toString("utf8") }));
      serial.on("error", (e) => send(ws, { type: "error", message: e.message }));
      serial.on("close", () => send(ws, { type: "closed" }));
      serial.on("open", () =>
        send(ws, { type: "opened", path: target.path, baud })
      );
      ws.__serial = serial;
    })
    .catch((e) => send(ws, { type: "error", message: `port list failed: ${e.message}` }));
}

wss.on("connection", (ws) => {
  let opening = false;
  let opened = false;

  ws.on("message", (data) => {
    if (!opened && !opening) {
      let msg;
      try {
        msg = JSON.parse(data.toString());
      } catch {
        return;
      }
      if (msg.type === "open") {
        opening = true;
        openSerial(ws, msg.baud || 115200);
        // Mark opened once the serial 'open' event fires; until then, ignore
        // further control frames and let the server echo any stray text.
        const check = setInterval(() => {
          if (ws.__serial && ws.__serial.isOpen) {
            opened = true;
            clearInterval(check);
          }
        }, 50);
      }
      return;
    }
    // After open, all text from the client is written to the serial port.
    if (ws.__serial && ws.__serial.writable) {
      ws.__serial.write(data.toString());
    }
  });

  ws.on("close", () => {
    try {
      ws.__serial && ws.__serial.close();
    } catch {
      /* ignore */
    }
  });
});

console.log(`CH347F serial bridge listening on ws://0.0.0.0:${BRIDGE_PORT}`);
