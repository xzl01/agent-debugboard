export const SERIAL_BROKER_PROTOCOL = "linkr-serial-broker.v1";

export const SERIAL_BROKER_CHANNELS = Object.freeze(["uart0", "uart1"]);

export const SERIAL_BROKER_CAPABILITIES = Object.freeze({
  channels: SERIAL_BROKER_CHANNELS,
  encodings: Object.freeze(["utf8", "base64"]),
  shared_read: true,
  ordered_write: true,
  exclusive_write: true,
  observer_redaction: "line_token",
});

let requestSequence = 0;

export function createSerialBrokerRequestId(prefix = "web") {
  requestSequence = (requestSequence + 1) >>> 0;
  return `${prefix}-${Date.now().toString(36)}-${requestSequence.toString(36)}`;
}

export function serialBrokerFrame(type, fields = {}) {
  return { protocol: SERIAL_BROKER_PROTOCOL, type, ...fields };
}

export function serialBrokerOpenRequest(channel, baud, requestId = createSerialBrokerRequestId()) {
  return serialBrokerFrame("open", {
    request_id: requestId,
    channel: normalizeSerialBrokerChannel(channel),
    baud: normalizeSerialBrokerBaud(baud),
  });
}

export function serialBrokerWriteRequest(
  channel,
  text,
  requestId = createSerialBrokerRequestId(),
  { observerRedactionToken } = {},
) {
  const request = serialBrokerFrame("write", {
    request_id: requestId,
    channel: normalizeSerialBrokerChannel(channel),
    data: { encoding: "utf8", value: String(text) },
  });
  if (observerRedactionToken != null) {
    request.observer_redaction = { line_token: normalizeObserverRedactionToken(observerRedactionToken) };
  }
  return request;
}

export function serialBrokerCloseRequest(channel, requestId = createSerialBrokerRequestId()) {
  return serialBrokerFrame("close", {
    request_id: requestId,
    channel: normalizeSerialBrokerChannel(channel),
  });
}

export function serialBrokerClaimRequest(
  channel,
  owner,
  requestId = createSerialBrokerRequestId(),
) {
  return serialBrokerFrame("claim", {
    request_id: requestId,
    channel: normalizeSerialBrokerChannel(channel),
    owner: String(owner || "automation").slice(0, 80),
  });
}

export function serialBrokerReleaseRequest(channel, requestId = createSerialBrokerRequestId()) {
  return serialBrokerFrame("release", {
    request_id: requestId,
    channel: normalizeSerialBrokerChannel(channel),
  });
}

export function normalizeSerialBrokerChannel(value) {
  return value === "uart1" ? "uart1" : "uart0";
}

export function normalizeSerialBrokerBaud(value) {
  const baud = Number(value);
  if (!Number.isSafeInteger(baud) || baud < 300 || baud > 4_000_000) {
    throw new Error("baud must be an integer between 300 and 4000000");
  }
  return baud;
}

function requestId(value) {
  return typeof value === "string" && value.length > 0 ? value.slice(0, 128) : undefined;
}

function invalid(code, message, requestIdValue) {
  return {
    ok: false,
    error: {
      code,
      message,
      request_id: requestIdValue,
    },
  };
}

function normalizeObserverRedactionToken(value) {
  const token = String(value);
  if (!/^[A-Za-z0-9]{8,64}$/.test(token)) {
    throw new Error("observer_redaction.line_token must be 8-64 ASCII letters or digits");
  }
  return token;
}

export function parseSerialBrokerClientFrame(raw) {
  const text = typeof raw === "string" ? raw : raw?.toString?.("utf8") ?? String(raw ?? "");
  let value;
  try {
    value = JSON.parse(text);
  } catch {
    return invalid("invalid_json", "broker request must be valid JSON");
  }

  if (!value || typeof value !== "object" || Array.isArray(value)) {
    return invalid("invalid_request", "broker request must be a JSON object");
  }
  const id = requestId(value.request_id);
  if (value.protocol !== SERIAL_BROKER_PROTOCOL) {
    return invalid(
      "unsupported_protocol",
      `expected broker protocol ${SERIAL_BROKER_PROTOCOL}`,
      id,
    );
  }

  if (!SERIAL_BROKER_CHANNELS.includes(value.channel)) {
    return invalid("invalid_channel", "channel must be uart0 or uart1", id);
  }
  const channel = value.channel;
  try {
    switch (value.type) {
      case "open":
        return {
          ok: true,
          message: {
            type: "open",
            request_id: id,
            channel,
            baud: normalizeSerialBrokerBaud(value.baud ?? 115200),
          },
        };
      case "write": {
        const encoding = value.data?.encoding ?? "utf8";
        const dataValue = value.data?.value ?? value.text;
        if ((encoding !== "utf8" && encoding !== "base64") || typeof dataValue !== "string") {
          return invalid("invalid_write", "write data must contain utf8 or base64 text", id);
        }
        let observerRedaction;
        if (value.observer_redaction != null) {
          if (encoding !== "utf8") {
            return invalid("invalid_write", "observer redaction is only supported for UTF-8 writes", id);
          }
          try {
            observerRedaction = {
              line_token: normalizeObserverRedactionToken(value.observer_redaction?.line_token),
            };
          } catch (error) {
            return invalid("invalid_write", error.message, id);
          }
        }
        return {
          ok: true,
          message: {
            type: "write",
            request_id: id,
            channel,
            data: { encoding, value: dataValue },
            ...(observerRedaction ? { observer_redaction: observerRedaction } : {}),
          },
        };
      }
      case "status":
      case "close":
      case "release":
        return {
          ok: true,
          message: { type: value.type, request_id: id, channel },
        };
      case "claim": {
        const owner = typeof value.owner === "string" && value.owner.trim()
          ? value.owner.trim().slice(0, 80)
          : "automation";
        return {
          ok: true,
          message: { type: "claim", request_id: id, channel, owner },
        };
      }
      default:
        return invalid("unsupported_request", `unsupported broker request: ${String(value.type)}`, id);
    }
  } catch (error) {
    return invalid(
      "invalid_request",
      error instanceof Error ? error.message : String(error),
      id,
    );
  }
}

export function parseSerialBrokerServerFrame(raw) {
  let value;
  try {
    value = JSON.parse(typeof raw === "string" ? raw : String(raw));
  } catch {
    return null;
  }
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  if (value.protocol !== SERIAL_BROKER_PROTOCOL) return null;
  return value;
}
