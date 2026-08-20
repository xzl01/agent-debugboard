import assert from "node:assert/strict";
import { test } from "node:test";
import { shouldEnableReactDiagnostics } from "./devtools.ts";

test("keeps diagnostics disabled by default in development", () => {
  assert.equal(shouldEnableReactDiagnostics(true, undefined), false);
});

test("keeps diagnostics disabled for an empty flag", () => {
  assert.equal(shouldEnableReactDiagnostics(true, ""), false);
});

test("enables diagnostics only for an explicit 0 flag", () => {
  assert.equal(shouldEnableReactDiagnostics(true, "0"), true);
});

test("keeps diagnostics disabled for other values", () => {
  assert.equal(shouldEnableReactDiagnostics(true, "false"), false);
});

test("disables diagnostics when the disable flag is exactly 1", () => {
  assert.equal(shouldEnableReactDiagnostics(true, "1"), false);
});

test("never enables diagnostics outside development", () => {
  assert.equal(shouldEnableReactDiagnostics(false, undefined), false);
  assert.equal(shouldEnableReactDiagnostics(false, "0"), false);
});
