import { apiEndpoint } from "./api.ts";

const SHA256_K = [
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

export const OTA_AUTO_CONFIRM_MS = 16_000;

export const OTA_STATES = [
  "idle",
  "uploading",
  "verified",
  "pending_test",
  "rebooting",
  "failed",
  "unknown",
] as const;

export type OtaState = (typeof OTA_STATES)[number];

export interface OtaFirmwareError {
  code?: string;
  errno?: number;
  message?: string;
}

export interface OtaStatus {
  state: OtaState;
  expectedSize: number | null;
  writtenSize: number | null;
  maxSize: number | null;
  currentImageConfirmed: boolean | null;
  lastError: OtaFirmwareError | null;
}

export interface OtaUploadProgress {
  loaded: number;
  total: number;
  percent: number;
}

export interface UploadOtaOptions {
  onProgress?: (progress: OtaUploadProgress) => void;
  signal?: AbortSignal;
  xhrFactory?: () => OtaUploadXhr;
}

export interface OtaCryptoLike {
  subtle?: Pick<SubtleCrypto, "digest">;
}

export interface OtaProgressEventLike {
  lengthComputable: boolean;
  loaded: number;
  total: number;
}

export interface OtaUploadXhr {
  status: number;
  statusText: string;
  responseText: string;
  onload: (() => void) | null;
  onerror: (() => void) | null;
  onabort: (() => void) | null;
  upload: {
    onprogress: ((event: OtaProgressEventLike) => void) | null;
  };
  open(method: string, url: string): void;
  setRequestHeader(name: string, value: string): void;
  send(body: Blob): void;
  abort(): void;
}

function createDefaultUploadXhr(): OtaUploadXhr {
  const xhr = new XMLHttpRequest();
  let onload: (() => void) | null = null;
  let onerror: (() => void) | null = null;
  let onabort: (() => void) | null = null;
  let onprogress: ((event: OtaProgressEventLike) => void) | null = null;

  const wrapped: OtaUploadXhr = {
    status: 0,
    statusText: "",
    responseText: "",
    onload: null,
    onerror: null,
    onabort: null,
    upload: {
      onprogress: null,
    },
    open(method, url) {
      xhr.open(method, url);
    },
    setRequestHeader(name, value) {
      xhr.setRequestHeader(name, value);
    },
    send(body) {
      xhr.send(body);
    },
    abort() {
      xhr.abort();
    },
  };

  Object.defineProperties(wrapped, {
    status: {
      get: () => xhr.status,
    },
    statusText: {
      get: () => xhr.statusText,
    },
    responseText: {
      get: () => xhr.responseText,
    },
    onload: {
      get: () => onload,
      set: (handler: (() => void) | null) => {
        onload = handler;
        xhr.onload = handler ? () => handler() : null;
      },
    },
    onerror: {
      get: () => onerror,
      set: (handler: (() => void) | null) => {
        onerror = handler;
        xhr.onerror = handler ? () => handler() : null;
      },
    },
    onabort: {
      get: () => onabort,
      set: (handler: (() => void) | null) => {
        onabort = handler;
        xhr.onabort = handler ? () => handler() : null;
      },
    },
  });

  Object.defineProperty(wrapped.upload, "onprogress", {
    get: () => onprogress,
    set: (handler: ((event: OtaProgressEventLike) => void) | null) => {
      onprogress = handler;
      xhr.upload.onprogress = handler ? (event) => handler(event) : null;
    },
  });

  return wrapped;
}

function rotateRight(value: number, shift: number): number {
  return (value >>> shift) | (value << (32 - shift));
}

function sha256Fallback(bytes: Uint8Array): string {
  const bitLength = bytes.length * 8;
  const paddedLength = (((bytes.length + 9 + 63) >> 6) << 6);
  const padded = new Uint8Array(paddedLength);
  padded.set(bytes);
  padded[bytes.length] = 0x80;

  const high = Math.floor(bitLength / 0x1_0000_0000);
  const low = bitLength >>> 0;
  padded[padded.length - 8] = (high >>> 24) & 0xff;
  padded[padded.length - 7] = (high >>> 16) & 0xff;
  padded[padded.length - 6] = (high >>> 8) & 0xff;
  padded[padded.length - 5] = high & 0xff;
  padded[padded.length - 4] = (low >>> 24) & 0xff;
  padded[padded.length - 3] = (low >>> 16) & 0xff;
  padded[padded.length - 2] = (low >>> 8) & 0xff;
  padded[padded.length - 1] = low & 0xff;

  const hash = new Uint32Array([
    0x6a09e667,
    0xbb67ae85,
    0x3c6ef372,
    0xa54ff53a,
    0x510e527f,
    0x9b05688c,
    0x1f83d9ab,
    0x5be0cd19,
  ]);
  const words = new Uint32Array(64);

  for (let offset = 0; offset < padded.length; offset += 64) {
    for (let index = 0; index < 16; index += 1) {
      const base = offset + index * 4;
      words[index] = (
        (padded[base] << 24) |
        (padded[base + 1] << 16) |
        (padded[base + 2] << 8) |
        padded[base + 3]
      ) >>> 0;
    }

    for (let index = 16; index < 64; index += 1) {
      const s0 = rotateRight(words[index - 15], 7) ^ rotateRight(words[index - 15], 18) ^ (words[index - 15] >>> 3);
      const s1 = rotateRight(words[index - 2], 17) ^ rotateRight(words[index - 2], 19) ^ (words[index - 2] >>> 10);
      words[index] = (words[index - 16] + s0 + words[index - 7] + s1) >>> 0;
    }

    let a = hash[0];
    let b = hash[1];
    let c = hash[2];
    let d = hash[3];
    let e = hash[4];
    let f = hash[5];
    let g = hash[6];
    let h = hash[7];

    for (let index = 0; index < 64; index += 1) {
      const s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
      const ch = (e & f) ^ (~e & g);
      const temp1 = (h + s1 + ch + SHA256_K[index] + words[index]) >>> 0;
      const s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const temp2 = (s0 + maj) >>> 0;

      h = g;
      g = f;
      f = e;
      e = (d + temp1) >>> 0;
      d = c;
      c = b;
      b = a;
      a = (temp1 + temp2) >>> 0;
    }

    hash[0] = (hash[0] + a) >>> 0;
    hash[1] = (hash[1] + b) >>> 0;
    hash[2] = (hash[2] + c) >>> 0;
    hash[3] = (hash[3] + d) >>> 0;
    hash[4] = (hash[4] + e) >>> 0;
    hash[5] = (hash[5] + f) >>> 0;
    hash[6] = (hash[6] + g) >>> 0;
    hash[7] = (hash[7] + h) >>> 0;
  }

  return Array.from(hash, (value) => value.toString(16).padStart(8, "0")).join("");
}

async function bytesFromInput(input: Blob | ArrayBuffer | ArrayBufferView): Promise<Uint8Array> {
  if (input instanceof Blob) {
    return new Uint8Array(await input.arrayBuffer());
  }

  if (input instanceof ArrayBuffer) {
    return new Uint8Array(input);
  }

  return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
}

export function resolveOtaUrl(path = "", base = apiEndpoint()): string {
  return `${base.replace(/\/$/, "")}/ota${path}`;
}

export class OtaApiError extends Error {
  code?: string;
  status?: number;

  constructor(message: string, code?: string, status?: number) {
    super(message);
    this.name = "OtaApiError";
    this.code = code;
    this.status = status;
  }
}

function invalidResponseError(status: number, statusText: string): OtaApiError {
  return new OtaApiError(
    `Device endpoint returned ${status} ${statusText} instead of valid OTA JSON. Start the local device gateway and verify the configured API endpoint.`,
    "invalid_response",
    status
  );
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function toFiniteNumber(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

function toPositiveFiniteNumber(value: unknown): number | null {
  const number = toFiniteNumber(value);
  return number != null && number > 0 ? number : null;
}

function toErrno(value: unknown): number | undefined {
  return typeof value === "number" && Number.isInteger(value) ? value : undefined;
}

function parseJsonText(text: string): unknown {
  if (!text.trim()) {
    return null;
  }

  try {
    return JSON.parse(text);
  } catch {
    return null;
  }
}

export function parseOtaSuccessResponse(
  text: string,
  status: number,
  statusText: string
): Record<string, unknown> {
  const payload = parseJsonText(text);
  if (!isRecord(payload)) {
    throw invalidResponseError(status, statusText);
  }

  return payload;
}

function parseLastError(value: unknown): OtaFirmwareError | null {
  if (typeof value === "string" && value.trim()) {
    return { message: value };
  }

  if (!isRecord(value)) {
    return null;
  }

  const message = typeof value.message === "string" ? value.message : undefined;
  const code = typeof value.code === "string" ? value.code : undefined;
  const errno = toErrno(value.errno);

  if (!message && !code && errno == null) {
    return null;
  }

  const error: OtaFirmwareError = {};
  if (message) {
    error.message = message;
  }
  if (code) {
    error.code = code;
  }
  if (errno != null) {
    error.errno = errno;
  }
  return error;
}

function parseStatusSource(payload: unknown): Record<string, unknown> | null {
  if (!isRecord(payload)) {
    return null;
  }

  if (isRecord(payload.status)) {
    return payload.status;
  }

  return payload;
}

function readResponseText(xhr: OtaUploadXhr): string {
  return typeof xhr.responseText === "string" ? xhr.responseText : "";
}

export function normalizeOtaState(value: unknown): OtaState {
  return typeof value === "string" && OTA_STATES.includes(value as OtaState)
    ? (value as OtaState)
    : "unknown";
}

export function normalizeOtaStatus(payload: unknown): OtaStatus {
  const source = parseStatusSource(payload);

  return {
    state: normalizeOtaState(source?.state),
    expectedSize: toFiniteNumber(source?.expected_size),
    writtenSize: toFiniteNumber(source?.written_size),
    maxSize: toPositiveFiniteNumber(source?.max_size),
    currentImageConfirmed:
      typeof source?.current_image_confirmed === "boolean"
        ? source.current_image_confirmed
        : null,
    lastError: parseLastError(source?.last_error),
  };
}

export function extractOtaError(payload: unknown): OtaFirmwareError | null {
  if (!isRecord(payload)) {
    return null;
  }

  const nested = payload.error;
  if (isRecord(nested)) {
    const message = typeof nested.message === "string" ? nested.message : undefined;
    const code = typeof nested.code === "string" ? nested.code : undefined;
    const errno = toErrno(nested.errno);
    if (message || code || errno != null) {
      const error: OtaFirmwareError = {};
      if (message) {
        error.message = message;
      }
      if (code) {
        error.code = code;
      }
      if (errno != null) {
        error.errno = errno;
      }
      return error;
    }
  }

  if (typeof payload.message === "string") {
    const error: OtaFirmwareError = { message: payload.message };
    if (typeof payload.code === "string") {
      error.code = payload.code;
    }
    const errno = toErrno(payload.errno);
    if (errno != null) {
      error.errno = errno;
    }
    return error;
  }

  if (typeof payload.code === "string" || toErrno(payload.errno) != null) {
    const error: OtaFirmwareError = {};
    if (typeof payload.code === "string") {
      error.code = payload.code;
    }
    const errno = toErrno(payload.errno);
    if (errno != null) {
      error.errno = errno;
    }
    return error;
  }

  return null;
}

export function formatOtaFirmwareError(error: OtaFirmwareError | null): string {
  if (!error) {
    return "";
  }

  const parts: string[] = [];
  if (error.code) {
    parts.push(error.code);
  }
  if (error.errno != null) {
    parts.push(`errno ${error.errno}`);
  }

  if (error.message) {
    return parts.length > 0 ? `${parts.join(" · ")}: ${error.message}` : error.message;
  }

  return parts.join(" · ");
}

export async function sha256HexFromBytes(
  bytes: Uint8Array,
  cryptoApi: OtaCryptoLike | null | undefined = globalThis.crypto
): Promise<string> {
  if (cryptoApi?.subtle) {
    const digest = await cryptoApi.subtle.digest("SHA-256", Uint8Array.from(bytes));
    return Array.from(new Uint8Array(digest), (byte) => byte.toString(16).padStart(2, "0")).join("");
  }

  return sha256Fallback(bytes);
}

export function createOtaApiError(
  status: number,
  statusText: string,
  payload: unknown
): OtaApiError {
  const extracted = extractOtaError(payload);
  return new OtaApiError(
    extracted?.message || formatOtaFirmwareError(extracted) || `HTTP ${status} ${statusText}`,
    extracted?.code,
    status
  );
}

export function buildOtaUploadHeaders(size: number, sha256: string): Record<string, string> {
  return {
    "Content-Type": "application/octet-stream",
    "X-Linkr-Ota-Size": String(size),
    "X-Linkr-Ota-Sha256": sha256,
  };
}

export function createOtaUploadProgress(loaded: number, total: number): OtaUploadProgress {
  const safeTotal = total > 0 ? total : Math.max(loaded, 1);
  const safeLoaded = Math.max(0, Math.min(loaded, safeTotal));

  return {
    loaded: safeLoaded,
    total: safeTotal,
    percent: Math.round((safeLoaded / safeTotal) * 100),
  };
}

export function canUploadOta(status: OtaStatus | null, busy: boolean): boolean {
  return (
    !!status &&
    !busy &&
    status.state !== "uploading" &&
    status.state !== "pending_test" &&
    status.state !== "rebooting" &&
    status.state !== "unknown"
  );
}

export function canStartOtaTest(status: OtaStatus | null, busy: boolean): boolean {
  return !!status && !busy && status.state === "verified";
}

export function canConfirmOta(status: OtaStatus | null, busy: boolean): boolean {
  return (
    !!status &&
    !busy &&
    status.state === "pending_test" &&
    status.currentImageConfirmed === false
  );
}

export function isOtaRebooting(status: OtaStatus | null): boolean {
  return status?.state === "rebooting";
}

export async function computeOtaSha256Hex(
  input: Blob | ArrayBuffer | ArrayBufferView,
  cryptoApi: OtaCryptoLike | null | undefined = globalThis.crypto
): Promise<string> {
  return sha256HexFromBytes(await bytesFromInput(input), cryptoApi);
}

async function otaRequest(path: string, init?: RequestInit): Promise<unknown> {
  let response: Response;

  try {
    response = await fetch(resolveOtaUrl(path), init);
  } catch (error) {
    throw new OtaApiError(
      `${error instanceof Error ? error.message : "Network request failed"}. ` +
        "If this page is hosted on GitHub Pages, start the local gateway with `npm run device-bridge`."
    );
  }

  const text = await response.text();
  const payload = parseJsonText(text);
  if (!response.ok || (isRecord(payload) && payload.ok === false)) {
    throw createOtaApiError(response.status, response.statusText, payload);
  }

  return parseOtaSuccessResponse(text, response.status, response.statusText);
}

export async function getOtaStatus(signal?: AbortSignal): Promise<OtaStatus> {
  return normalizeOtaStatus(await otaRequest("", { signal }));
}

export async function startOtaTest(signal?: AbortSignal): Promise<void> {
  await otaRequest("/test", { method: "POST", signal });
}

export async function confirmOtaImage(signal?: AbortSignal): Promise<void> {
  await otaRequest("/confirm", { method: "POST", signal });
}

export function parseOtaResponseText(text: string): unknown {
  return parseJsonText(text);
}

export async function uploadOtaImage(
  file: File,
  sha256: string,
  options: UploadOtaOptions = {}
): Promise<OtaStatus> {
  const xhr = options.xhrFactory ? options.xhrFactory() : createDefaultUploadXhr();
  const headers = buildOtaUploadHeaders(file.size, sha256);

  return await new Promise<OtaStatus>((resolve, reject) => {
    let settled = false;

    const settle = (callback: () => void) => {
      if (settled) {
        return;
      }

      settled = true;
      if (options.signal && abortHandler) {
        options.signal.removeEventListener("abort", abortHandler);
      }
      callback();
    };

    const abortHandler = () => {
      xhr.abort();
    };

    xhr.open("POST", resolveOtaUrl("/upload"));
    for (const [name, value] of Object.entries(headers)) {
      xhr.setRequestHeader(name, value);
    }

    xhr.upload.onprogress = (event: OtaProgressEventLike) => {
      const total = event.lengthComputable && event.total > 0 ? event.total : file.size;
      options.onProgress?.(createOtaUploadProgress(event.loaded, total));
    };

    xhr.onerror = () => {
      settle(() => {
        reject(
          new OtaApiError(
            "Network request failed while uploading OTA image. If this page is hosted on GitHub Pages, start the local gateway with `npm run device-bridge`."
          )
        );
      });
    };

    xhr.onabort = () => {
      settle(() => {
        reject(new OtaApiError("OTA upload aborted.", "aborted"));
      });
    };

    xhr.onload = () => {
      settle(() => {
        const text = readResponseText(xhr);
        const payload = parseOtaResponseText(text);
        if (xhr.status < 200 || xhr.status >= 300) {
          reject(createOtaApiError(xhr.status, xhr.statusText, payload));
          return;
        }

        try {
          resolve(normalizeOtaStatus(parseOtaSuccessResponse(text, xhr.status, xhr.statusText)));
        } catch (error) {
          reject(error);
        }
      });
    };

    if (options.signal?.aborted) {
      abortHandler();
      return;
    }

    if (options.signal) {
      options.signal.addEventListener("abort", abortHandler, { once: true });
    }

    xhr.send(file);
  });
}
