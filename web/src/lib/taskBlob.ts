import { TASK_MAX_REQUESTS, TASK_MAX_WAIT_MS, type TaskRequest } from "./taskRequests.ts";

export const TASK_BLOB_MARKER = "# linkr-task.v1";
const TASK_ID_MARKER = "# task ";
export const TASK_BLOB_MAX_BYTES = 4096;
export const TASK_MAX_TASKS = 4;
export const TASK_ID_MAX_BYTES = 31;
export const TASK_NAME_MAX_BYTES = 63;
export const TASK_LINE_MAX_BYTES = 256;
export const TASK_PATH_MAX_BYTES = 96;
export const TASK_BODY_MAX_BYTES = 192;
export const TASK_MAX_JSON_DEPTH = 16;
const ALLOWED_PATH_PREFIXES = ["/api/v1/power/", "/api/v1/gpio/", "/api/v1/switch/"] as const;
const REQUEST_KEYS = ["method", "path", "body", "wait_ms"] as const;

const textEncoder = new TextEncoder();
export const taskByteLength = (value: string): number => textEncoder.encode(value).length;

export type TaskDataErrorCode = "invalid_response" | "invalid_blob" | "unknown_task" | "catalog_unavailable";

export class TaskDataError extends Error {
  readonly code: TaskDataErrorCode;
  readonly detail: string;

  constructor(code: TaskDataErrorCode, detail: string) {
    super(detail);
    this.name = "TaskDataError";
    this.code = code;
    this.detail = detail;
  }
}

export interface ParsedTask {
  readonly id: string;
  readonly requests: readonly TaskRequest[];
}

export function isTaskControlPath(path: string): boolean {
  if (taskByteLength(path) > TASK_PATH_MAX_BYTES) return false;
  const prefix = ALLOWED_PATH_PREFIXES.find((candidate) => path.startsWith(candidate));
  if (prefix === undefined) return false;
  const identifier = path.slice(prefix.length);
  if (identifier === "" || identifier === "." || identifier === "..") return false;
  for (const character of identifier) {
    const codePoint = character.codePointAt(0);
    if (
      codePoint === undefined ||
      codePoint <= 0x20 ||
      codePoint >= 0x7f ||
      character === "/" ||
      character === "\\" ||
      character === "%" ||
      character === "?" ||
      character === "#"
    ) {
      return false;
    }
  }
  return true;
}

function asRecord(value: unknown): Record<string, unknown> | null {
  return typeof value === "object" && value !== null && !Array.isArray(value)
    ? Object.fromEntries(Object.entries(value))
    : null;
}

function decodedString(token: string): string {
  const value: unknown = JSON.parse(token);
  if (typeof value !== "string") {
    throw new TaskDataError("invalid_blob", "request envelope key is not a string");
  }
  return value;
}

function topLevelKeys(line: string): readonly string[] {
  const keys: string[] = [];
  let depth = 0;
  let index = 0;
  while (index < line.length) {
    const character = line[index];
    if (character === '"') {
      const start = index;
      index += 1;
      while (index < line.length) {
        if (line[index] === "\\") {
          index += 2;
        } else if (line[index] === '"') {
          index += 1;
          break;
        } else {
          index += 1;
        }
      }
      let cursor = index;
      while (cursor < line.length && /\s/.test(line[cursor] ?? "")) cursor += 1;
      if (depth === 1 && line[cursor] === ":") {
        keys.push(decodedString(line.slice(start, index)));
      }
      continue;
    }
    if (character === "{" || character === "[") depth += 1;
    if (character === "}" || character === "]") depth -= 1;
    index += 1;
  }
  return keys;
}

function requestEnvelopeValid(line: string): boolean {
  const keys = topLevelKeys(line);
  if (keys.length < 3 || keys.length > 4) return false;
  const seen = new Set<string>();
  for (const key of keys) {
    if (!REQUEST_KEYS.some((candidate) => candidate === key) || seen.has(key)) return false;
    seen.add(key);
  }
  return seen.has("method") && seen.has("path") && seen.has("body");
}

export function taskJsonDepthValid(body: string): boolean {
  let depth = 0;
  let inString = false;
  let escaped = false;
  for (const character of body) {
    if (inString) {
      if (escaped) escaped = false;
      else if (character === "\\") escaped = true;
      else if (character === '"') inString = false;
      continue;
    }
    if (character === '"') inString = true;
    else if (character === "{" || character === "[") {
      depth += 1;
      if (depth > TASK_MAX_JSON_DEPTH) return false;
    } else if (character === "}" || character === "]") {
      depth -= 1;
    }
  }
  return true;
}

function parseRequestLine(line: string): TaskRequest {
  let value: unknown;
  try {
    value = JSON.parse(line);
  } catch (cause) {
    throw new TaskDataError(
      "invalid_blob",
      `invalid request line: ${cause instanceof Error ? cause.message : String(cause)}`,
    );
  }
  const record = asRecord(value);
  if (!record) throw new TaskDataError("invalid_blob", "request line is not a JSON object");
  if (!requestEnvelopeValid(line)) {
    throw new TaskDataError("invalid_blob", "request line must contain only unique supported fields");
  }
  if (record.method !== "PUT") throw new TaskDataError("invalid_blob", "stored requests must use method PUT");
  const path = record.path;
  if (typeof path === "string" && taskByteLength(path) > TASK_PATH_MAX_BYTES) {
    throw new TaskDataError("invalid_blob", `path exceeds the ${TASK_PATH_MAX_BYTES} byte firmware limit`);
  }
  if (typeof path !== "string" || !isTaskControlPath(path)) {
    throw new TaskDataError("invalid_blob", `path ${JSON.stringify(path)} is not an allowed control path`);
  }
  const body = record.body;
  if (typeof body !== "string") throw new TaskDataError("invalid_blob", "request line is missing string body");
  if (taskByteLength(body) > TASK_BODY_MAX_BYTES) {
    throw new TaskDataError("invalid_blob", `body exceeds the ${TASK_BODY_MAX_BYTES} byte firmware limit`);
  }
  try {
    JSON.parse(body);
  } catch (cause) {
    if (cause instanceof Error) {
      throw new TaskDataError("invalid_blob", "request body is not valid JSON");
    }
    throw cause;
  }
  if (!taskJsonDepthValid(body)) throw new TaskDataError("invalid_blob", "request body exceeds the JSON depth limit");
  if (record.wait_ms === undefined) return { method: "PUT", path, body };
  if (
    typeof record.wait_ms !== "number" ||
    !Number.isInteger(record.wait_ms) ||
    record.wait_ms < 0 ||
    record.wait_ms > TASK_MAX_WAIT_MS
  ) {
    throw new TaskDataError("invalid_blob", `wait_ms must be an integer within 0..${TASK_MAX_WAIT_MS}`);
  }
  return { method: "PUT", path, body, wait_ms: record.wait_ms };
}

function taskIdValid(id: string): boolean {
  if (taskByteLength(id) === 0 || taskByteLength(id) > TASK_ID_MAX_BYTES) return false;
  return !/[ \t\r\n#]/.test(id);
}

export function parseTaskBlob(blob: string): ParsedTask[] {
  if (blob === "") return [];
  if (taskByteLength(blob) > TASK_BLOB_MAX_BYTES) {
    throw new TaskDataError("invalid_blob", `blob exceeds the ${TASK_BLOB_MAX_BYTES} byte firmware limit`);
  }
  const tasks: { id: string; requests: TaskRequest[] }[] = [];
  let current: { id: string; requests: TaskRequest[] } | null = null;
  let markerSeen = false;
  for (const rawLine of blob.split("\n")) {
    const line = rawLine.replace(/\r$/, "");
    if (line === "") continue;
    if (taskByteLength(line) > TASK_LINE_MAX_BYTES) {
      throw new TaskDataError("invalid_blob", `blob line exceeds the ${TASK_LINE_MAX_BYTES} byte firmware limit`);
    }
    if (!markerSeen) {
      if (line !== TASK_BLOB_MARKER) {
        throw new TaskDataError("invalid_blob", `blob must start with '${TASK_BLOB_MARKER}'`);
      }
      markerSeen = true;
      continue;
    }
    if (line.startsWith(TASK_ID_MARKER)) {
      const id = line.slice(TASK_ID_MARKER.length);
      if (!taskIdValid(id)) throw new TaskDataError("invalid_blob", "task header carries an invalid task id");
      if (tasks.length >= TASK_MAX_TASKS) {
        throw new TaskDataError("invalid_blob", `blob holds more than ${TASK_MAX_TASKS} tasks`);
      }
      if (tasks.some((task) => task.id === id)) {
        throw new TaskDataError("invalid_blob", `blob carries duplicate task id ${JSON.stringify(id)}`);
      }
      current = { id, requests: [] };
      tasks.push(current);
      continue;
    }
    if (line.startsWith("#")) continue;
    if (!current) throw new TaskDataError("invalid_blob", "request line outside any task section");
    if (current.requests.length >= TASK_MAX_REQUESTS) {
      throw new TaskDataError("invalid_blob", `task '${current.id}' holds more than ${TASK_MAX_REQUESTS} requests`);
    }
    current.requests.push(parseRequestLine(line));
  }
  if (!markerSeen) throw new TaskDataError("invalid_blob", `blob must start with '${TASK_BLOB_MARKER}'`);
  return tasks;
}
