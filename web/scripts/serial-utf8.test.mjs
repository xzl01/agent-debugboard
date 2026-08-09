import test from "node:test";
import assert from "node:assert/strict";

import { createSerialUtf8Decoder } from "./serial-utf8.mjs";

test("preserves UTF-8 characters split across serial chunks", () => {
  const decoder = createSerialUtf8Decoder();
  const bytes = Buffer.from("ready，完成。", "utf8");
  const pieces = [
    bytes.subarray(0, 7),
    bytes.subarray(7, 8),
    bytes.subarray(8, 12),
    bytes.subarray(12),
  ];
  const output = pieces.map((piece) => decoder.decode(piece)).join("") + decoder.flush();
  assert.equal(output, "ready，完成。");
  assert.equal(output.includes("�"), false);
});
