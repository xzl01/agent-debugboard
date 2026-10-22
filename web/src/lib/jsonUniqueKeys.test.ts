import assert from "node:assert/strict";
import test from "node:test";
import { assertUniqueJsonObjectKeys } from "./jsonUniqueKeys.ts";

test("validates arrays/scalars/Unicode and ignores JSON punctuation inside string values", () => {
  const values = [null, true, false, 0, -1.25e30, "", "文字😀", [1, null, true, { a: "{} : []", b: '\\"key\": {"key":2}' }], { a: { key: 1 }, b: [{ key: 2 }, { key: 3 }] }];
  for (const value of values) assert.doesNotThrow(() => assertUniqueJsonObjectKeys(JSON.stringify(value)));
  for (const json of [String.raw`{"\u0061":1,"b":2}`, String.raw`{"a":"\b\f\n\r\t\/\\\"","b":"\uD83D\uDE00"}`, " \t[ -0, 1e-2, 1.5E+2 ]\r\n"]) {
    assert.doesNotThrow(() => assertUniqueJsonObjectKeys(json));
    assert.doesNotThrow(() => JSON.parse(json));
  }
});

test("rejects duplicate decoded keys at every object/array depth, including escaped equivalents", () => {
  for (const json of [
    '{"a":1,"a":2}',
    '[{"a":{"x":1,"x":2}}]',
    String.raw`{"params":1,"pa\u0072ams":2}`,
    String.raw`{"😀":1,"\uD83D\uDE00":2}`,
    String.raw`{"a/b":1,"a\/b":2}`,
    String.raw`{"quote\"key":1,"quote\u0022key":2}`,
    '{"__proto__":1,"__proto__":2}',
    '{"constructor":1,"constructor":2}',
  ]) assert.throws(() => assertUniqueJsonObjectKeys(json), /Duplicate JSON key/);
});

test("rejects malformed JSON instead of tokenizing across invalid string/number/container boundaries", () => {
  for (const json of ["", "undefined", "NaN", "01", "+1", "1.", "1e", "truefalse", "{}{}", '{"a":1,}', "[1,]", '[1 2]', '{a:1}', '{"a" 1}', String.raw`"\x20"`, String.raw`"\u123"`, '"unterminated', '"line\nbreak"', "\u00a0null", '{"a":[]']) {
    assert.throws(() => assertUniqueJsonObjectKeys(json), SyntaxError, json);
  }
});
