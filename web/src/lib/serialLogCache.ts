export type SerialLogChannel = "uart0" | "uart1";

export const SERIAL_LOG_MAX_BYTES = 16 * 1024 * 1024;
export const SERIAL_LOG_RESTORE_BYTES = 1024 * 1024;

const DB_NAME = "radxa-linkr-debugger-serial-logs";
const DB_VERSION = 1;
const CHUNK_STORE = "chunks";
const META_STORE = "meta";

interface SerialLogChunk {
  id?: number;
  channel: SerialLogChannel;
  createdAt: number;
  text: string;
  byteLength: number;
}

interface SerialLogMeta {
  channel: SerialLogChannel;
  totalBytes: number;
}

export interface SerialLogSnapshot {
  text: string;
  totalBytes: number;
}

let databasePromise: Promise<IDBDatabase> | null = null;

function openDatabase(): Promise<IDBDatabase> {
  if (typeof indexedDB === "undefined") {
    return Promise.reject(new Error("IndexedDB is unavailable"));
  }
  if (databasePromise) return databasePromise;

  databasePromise = new Promise((resolve, reject) => {
    const request = indexedDB.open(DB_NAME, DB_VERSION);
    request.onupgradeneeded = () => {
      const database = request.result;
      if (!database.objectStoreNames.contains(CHUNK_STORE)) {
        const chunks = database.createObjectStore(CHUNK_STORE, {
          keyPath: "id",
          autoIncrement: true,
        });
        chunks.createIndex("channel", "channel", { unique: false });
      }
      if (!database.objectStoreNames.contains(META_STORE)) {
        database.createObjectStore(META_STORE, { keyPath: "channel" });
      }
    };
    request.onsuccess = () => {
      const database = request.result;
      database.onversionchange = () => {
        database.close();
        databasePromise = null;
      };
      resolve(database);
    };
    request.onerror = () => {
      databasePromise = null;
      reject(request.error ?? new Error("Failed to open serial log cache"));
    };
    request.onblocked = () => {
      databasePromise = null;
      reject(new Error("Serial log cache upgrade is blocked"));
    };
  });

  return databasePromise;
}

function requestResult<T>(request: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error("IndexedDB request failed"));
  });
}

function transactionComplete(transaction: IDBTransaction): Promise<void> {
  return new Promise((resolve, reject) => {
    transaction.oncomplete = () => resolve();
    transaction.onerror = () =>
      reject(transaction.error ?? new Error("Serial log cache transaction failed"));
    transaction.onabort = () =>
      reject(transaction.error ?? new Error("Serial log cache transaction was aborted"));
  });
}

export function utf8ByteLength(text: string): number {
  return new TextEncoder().encode(text).byteLength;
}

export function tailTextByUtf8Bytes(text: string, maxBytes: number): string {
  if (maxBytes <= 0 || !text) return "";
  const encoded = new TextEncoder().encode(text);
  if (encoded.byteLength <= maxBytes) return text;
  const decoder = new TextDecoder("utf-8", { fatal: true });
  let start = encoded.byteLength - maxBytes;
  while (start < encoded.byteLength) {
    try {
      return decoder.decode(encoded.slice(start));
    } catch {
      start += 1;
    }
  }
  return "";
}

export function serialLogFilename(channel: SerialLogChannel, date = new Date()): string {
  const timestamp = date.toISOString().replaceAll(":", "-").replace(".000Z", "Z");
  return `radxa-linkr-${channel}-${timestamp}.log`;
}

export async function appendSerialLogChunk(
  channel: SerialLogChannel,
  text: string
): Promise<number> {
  if (!text) return getSerialLogSize(channel);

  const retainedText = tailTextByUtf8Bytes(text, SERIAL_LOG_MAX_BYTES);
  const byteLength = utf8ByteLength(retainedText);
  const database = await openDatabase();
  const transaction = database.transaction([CHUNK_STORE, META_STORE], "readwrite");
  const chunks = transaction.objectStore(CHUNK_STORE);
  const meta = transaction.objectStore(META_STORE);
  let totalBytes = byteLength;

  const metaRequest = meta.get(channel) as IDBRequest<SerialLogMeta | undefined>;
  metaRequest.onsuccess = () => {
    totalBytes = (metaRequest.result?.totalBytes ?? 0) + byteLength;
    chunks.add({
      channel,
      createdAt: Date.now(),
      text: retainedText,
      byteLength,
    } satisfies SerialLogChunk);

    const writeMeta = () => meta.put({ channel, totalBytes } satisfies SerialLogMeta);
    if (totalBytes <= SERIAL_LOG_MAX_BYTES) {
      writeMeta();
      return;
    }

    const cursorRequest = chunks.index("channel").openCursor(IDBKeyRange.only(channel));
    cursorRequest.onsuccess = () => {
      const cursor = cursorRequest.result;
      if (!cursor || totalBytes <= SERIAL_LOG_MAX_BYTES) {
        writeMeta();
        return;
      }
      const chunk = cursor.value as SerialLogChunk;
      totalBytes = Math.max(0, totalBytes - chunk.byteLength);
      cursor.delete();
      cursor.continue();
    };
  };

  await transactionComplete(transaction);
  return totalBytes;
}

export async function readSerialLog(
  channel: SerialLogChannel,
  tailBytes?: number
): Promise<SerialLogSnapshot> {
  const database = await openDatabase();
  const transaction = database.transaction([CHUNK_STORE, META_STORE], "readonly");
  const chunksIndex = transaction.objectStore(CHUNK_STORE).index("channel");
  const chunksPromise = tailBytes == null
    ? requestResult(
        chunksIndex.getAll(IDBKeyRange.only(channel)) as IDBRequest<SerialLogChunk[]>
      )
    : new Promise<SerialLogChunk[]>((resolve, reject) => {
        if (tailBytes <= 0) {
          resolve([]);
          return;
        }

        const newestChunks: SerialLogChunk[] = [];
        let collectedBytes = 0;
        const cursorRequest = chunksIndex.openCursor(IDBKeyRange.only(channel), "prev");
        cursorRequest.onerror = () => reject(
          cursorRequest.error ?? new Error("Failed to read serial log cache")
        );
        cursorRequest.onsuccess = () => {
          const cursor = cursorRequest.result;
          if (!cursor) {
            resolve(newestChunks.reverse());
            return;
          }

          const chunk = cursor.value as SerialLogChunk;
          const remainingBytes = tailBytes - collectedBytes;
          if (chunk.byteLength > remainingBytes) {
            const text = tailTextByUtf8Bytes(chunk.text, remainingBytes);
            if (text) {
              newestChunks.push({
                ...chunk,
                text,
                byteLength: utf8ByteLength(text),
              });
            }
            resolve(newestChunks.reverse());
            return;
          }

          newestChunks.push(chunk);
          collectedBytes += chunk.byteLength;
          if (collectedBytes >= tailBytes) {
            resolve(newestChunks.reverse());
            return;
          }
          cursor.continue();
        };
      });
  const metaRequest = transaction
    .objectStore(META_STORE)
    .get(channel) as IDBRequest<SerialLogMeta | undefined>;
  const [chunks, meta] = await Promise.all([
    chunksPromise,
    requestResult(metaRequest),
    transactionComplete(transaction),
  ]);
  const text = chunks.map((chunk) => chunk.text).join("");
  const totalBytes = meta?.totalBytes ?? chunks.reduce(
    (total, chunk) => total + chunk.byteLength,
    0
  );
  return {
    text,
    totalBytes,
  };
}

export async function getSerialLogSize(channel: SerialLogChannel): Promise<number> {
  const database = await openDatabase();
  const transaction = database.transaction(META_STORE, "readonly");
  const request = transaction.objectStore(META_STORE).get(channel) as IDBRequest<
    SerialLogMeta | undefined
  >;
  const [meta] = await Promise.all([
    requestResult(request),
    transactionComplete(transaction),
  ]);
  return meta?.totalBytes ?? 0;
}

export async function clearSerialLog(channel: SerialLogChannel): Promise<void> {
  const database = await openDatabase();
  const transaction = database.transaction([CHUNK_STORE, META_STORE], "readwrite");
  const chunks = transaction.objectStore(CHUNK_STORE);
  const cursorRequest = chunks.index("channel").openCursor(IDBKeyRange.only(channel));
  cursorRequest.onsuccess = () => {
    const cursor = cursorRequest.result;
    if (!cursor) return;
    cursor.delete();
    cursor.continue();
  };
  transaction.objectStore(META_STORE).put({ channel, totalBytes: 0 } satisfies SerialLogMeta);
  await transactionComplete(transaction);
}
