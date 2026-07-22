import assert from "node:assert/strict";
import test from "node:test";

import {
  RIGOL_LIVE_SAMPLES,
  RIGOL_TRIGGER_SAMPLE,
  ScpiStreamReader,
  formatScpiNumber,
  repackFrameBits,
  rigolSlopeForTrigger,
  rigolSourceIndexForPin,
  timebaseForRate,
} from "./scpiScope.ts";

test("ScpiStreamReader parses text lines across chunk boundaries", () => {
  const reader = new ScpiStreamReader();
  const enc = new TextEncoder();
  assert.deepEqual(reader.feed(enc.encode("Rigol Techn")), []);
  const events = reader.feed(enc.encode("ologies,DS1102D,x,0.04\nRUN\n"));
  assert.deepEqual(events, [
    { type: "line", text: "Rigol Technologies,DS1102D,x,0.04" },
    { type: "line", text: "RUN" },
  ]);
});

test("ScpiStreamReader parses IEEE488 blocks split across chunks", () => {
  const reader = new ScpiStreamReader();
  const frame = new Uint8Array(1200);
  frame[601] = 0xab;
  const header = new TextEncoder().encode("#41200");
  assert.deepEqual(reader.feed(header.slice(0, 3)), []);
  assert.deepEqual(reader.feed(header.slice(3)), []);
  assert.deepEqual(reader.feed(frame.slice(0, 700)), []);
  const events = reader.feed(frame.slice(700));
  assert.equal(events.length, 1);
  assert.equal(events[0].type, "block");
  if (events[0].type === "block") {
    assert.equal(events[0].payload.length, 1200);
    assert.equal(events[0].payload[601], 0xab);
  }
});

test("ScpiStreamReader keeps trailing partial data for the next feed", () => {
  const reader = new ScpiStreamReader();
  const enc = new TextEncoder();
  const events = reader.feed(enc.encode("TD\n#36"));
  assert.deepEqual(events, [{ type: "line", text: "TD" }]);
  const block = reader.feed(enc.encode("00\x01\x02\x03"));
  assert.deepEqual(block, [{ type: "block", payload: new Uint8Array([1, 2, 3]) }]);
});

test("timebaseForRate follows the 600-samples-per-12-divisions relation", () => {
  assert.equal(timebaseForRate(1_000_000), 0.00005);
  assert.equal(timebaseForRate(2_500_000), 0.00002);
});

test("formatScpiNumber emits exponent notation the firmware parser accepts", () => {
  assert.match(formatScpiNumber(0.00005), /^5\.0000000e-5$/);
});

test("rigolSourceIndexForPin maps J16 connector order to channel indexes", () => {
  assert.equal(rigolSourceIndexForPin(10), 0);
  assert.equal(rigolSourceIndexForPin(17), 3);
  assert.equal(rigolSourceIndexForPin(29), 11);
  assert.equal(rigolSourceIndexForPin(9), 14);
  assert.equal(rigolSourceIndexForPin(99), 0);
});

test("rigolSlopeForTrigger maps rising to POS and other edges to NEG", () => {
  assert.equal(rigolSlopeForTrigger("rising"), "POS");
  assert.equal(rigolSlopeForTrigger("falling"), "NEG");
  assert.equal(rigolSlopeForTrigger("either"), "NEG");
});

test("repackFrameBits converts rigol channel order into selected-pin order", () => {
  const frame = new Uint16Array(RIGOL_LIVE_SAMPLES);
  frame[0] = 0b1010;
  frame[1] = 0b0100;
  const out = repackFrameBits(frame, [8, 10]);
  assert.equal(out[0], 0b10);
  assert.equal(out[1], 0b01);
  assert.equal(out.length, RIGOL_LIVE_SAMPLES);
  assert.equal(RIGOL_TRIGGER_SAMPLE, 300);
});
