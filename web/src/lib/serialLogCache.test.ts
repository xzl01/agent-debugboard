import test from "node:test";
import assert from "node:assert/strict";
import { IDBKeyRange, indexedDB } from "fake-indexeddb";

import {
  appendSerialLogChunk,
  clearSerialLog,
  readSerialLog,
  serialLogFilename,
  tailTextByUtf8Bytes,
  utf8ByteLength,
} from "./serialLogCache.ts";

Object.defineProperty(globalThis, "indexedDB", {
  configurable: true,
  value: indexedDB,
});
Object.defineProperty(globalThis, "IDBKeyRange", {
  configurable: true,
  value: IDBKeyRange,
});

test("keeps the newest UTF-8 serial log text within the byte limit", () => {
  const tail = tailTextByUtf8Bytes("boot\n内核启动\nlogin: ", 16);

  assert.equal(utf8ByteLength(tail) <= 16, true);
  assert.equal(tail.endsWith("login: "), true);
});

test("keeps complete serial log text when it already fits", () => {
  assert.equal(tailTextByUtf8Bytes("U-Boot ready\n", 128), "U-Boot ready\n");
  assert.equal(tailTextByUtf8Bytes("data", 0), "");
});

test("builds channel-specific timestamped log filenames", () => {
  assert.equal(
    serialLogFilename("uart1", new Date("2026-07-23T02:04:09.000Z")),
    "radxa-linkr-uart1-2026-07-23T02-04-09Z.log"
  );
});

test("appends, tails, and clears cached serial logs", async () => {
  await clearSerialLog("uart0");
  await appendSerialLogChunk("uart0", "old-");
  const totalBytes = await appendSerialLogChunk("uart0", "新-data");

  const tail = await readSerialLog("uart0", 5);
  assert.equal(tail.text, "-data");
  assert.equal(tail.totalBytes, totalBytes);

  const full = await readSerialLog("uart0");
  assert.equal(full.text, "old-新-data");
  assert.equal(full.totalBytes, utf8ByteLength(full.text));

  await clearSerialLog("uart0");
  assert.deepEqual(await readSerialLog("uart0"), { text: "", totalBytes: 0 });
});
