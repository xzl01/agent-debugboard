import test from "node:test";
import assert from "node:assert/strict";

import {
  DEFAULT_SERIAL_LINE_ENDING,
  normalizeConsolePunctuation,
  normalizeSerialTerminalInput,
} from "./serialInput.ts";

test("normalizes Chinese and full-width command punctuation to ASCII", () => {
  assert.equal(
    normalizeConsolePunctuation("echo（a，b）。！？：；【ok】"),
    "echo(a,b).!?:;[ok]"
  );
  assert.equal(normalizeConsolePunctuation("ｕｎａｍｅ　－ａ"), "uname -a");
});

test("keeps non-punctuation Unicode text intact", () => {
  assert.equal(normalizeConsolePunctuation("中文-test"), "中文-test");
});

test("normalizes keyboard and pasted newlines to the selected line ending", () => {
  assert.equal(normalizeSerialTerminalInput("one\rtwo\nthree\r\nfour", "crlf"), "one\r\ntwo\r\nthree\r\nfour");
  assert.equal(normalizeSerialTerminalInput("one\r\ntwo", "cr"), "one\rtwo");
  assert.equal(normalizeSerialTerminalInput("one\rtwo", "lf"), "one\ntwo");
});

test("uses one carriage return for interactive console Enter by default", () => {
  assert.equal(DEFAULT_SERIAL_LINE_ENDING, "cr");
  assert.equal(normalizeSerialTerminalInput("\r", DEFAULT_SERIAL_LINE_ENDING), "\r");
  assert.equal(new TextEncoder().encode(normalizeSerialTerminalInput("\r", DEFAULT_SERIAL_LINE_ENDING)).byteLength, 1);
});

test("problematic punctuation becomes one transmitted UTF-8 byte", () => {
  const encoder = new TextEncoder();
  assert.equal(encoder.encode(normalizeSerialTerminalInput("。", "cr")).byteLength, 1);
  assert.equal(encoder.encode(normalizeSerialTerminalInput("，", "cr")).byteLength, 1);
});
