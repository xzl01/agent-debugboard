import assert from "node:assert/strict";
import { test } from "node:test";
import { shouldEnableReactDiagnostics } from "./devtools.ts";

test("enables diagnostics only in development with the disable flag unset", () => {
  assert.equal(shouldEnableReactDiagnostics(true, undefined), true);
});

test("treats an empty disable flag as enabled", () => {
  assert.equal(shouldEnableReactDiagnostics(true, ""), true);
});

test("treats an explicit 0 disable flag as enabled", () => {
  assert.equal(shouldEnableReactDiagnostics(true, "0"), true);
});

test("treats any non-1 disable flag value as enabled", () => {
  assert.equal(shouldEnableReactDiagnostics(true, "false"), true);
});

test("disables diagnostics when the disable flag is exactly 1", () => {
  assert.equal(shouldEnableReactDiagnostics(true, "1"), false);
});

test("never enables diagnostics outside development", () => {
  assert.equal(shouldEnableReactDiagnostics(false, undefined), false);
  assert.equal(shouldEnableReactDiagnostics(false, "0"), false);
});
