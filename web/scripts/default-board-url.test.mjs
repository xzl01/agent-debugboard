import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const cases = [
  [new URL("../vite.config.ts", import.meta.url), '"http://172.29.203.1"'],
  [new URL("./dev-server.mjs", import.meta.url), '"http://172.29.203.1"'],
  [new URL("../../host-tools/src/config.rs", import.meta.url), '"http://172.29.203.1"'],
];

test("local Web entry points default to the firmware port 80 service", async () => {
  for (const [sourceUrl, expectedDefault] of cases) {
    const source = await readFile(sourceUrl, "utf8");
    assert.match(source, new RegExp(expectedDefault.replaceAll(".", "\\.")));
    assert.doesNotMatch(source, /172\.29\.203\.1:8080/);
  }
});
