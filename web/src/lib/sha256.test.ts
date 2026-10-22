import assert from "node:assert/strict";
import test from "node:test";
import { createHash } from "node:crypto";
import { sha256Hex } from "./sha256.ts";

test("SHA-256 HTTP-origin fallback matches native hashing across block and wire boundaries", async () => {
  for (const size of [0, 3, 55, 56, 63, 64, 65, 4000, 8192]) {
    const bytes = Uint8Array.from({ length: size }, (_, i) => (i * 17) % 256);
    const expected = createHash("sha256").update(bytes).digest("hex");
    assert.equal(await sha256Hex(bytes, null), expected);
    assert.equal(await sha256Hex(bytes), expected);
  }
});
