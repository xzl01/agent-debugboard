import test from "node:test";
import assert from "node:assert/strict";

import {
  isLinuxSerialHost,
  shouldShowLinuxSerialPermissionHelp,
} from "./serialPermissions.ts";

test("detects Linux from browser platform metadata", () => {
  assert.equal(isLinuxSerialHost({ userAgentData: { platform: "Linux" } }), true);
  assert.equal(isLinuxSerialHost({ platform: "Linux aarch64" }), true);
  assert.equal(isLinuxSerialHost({ userAgent: "Mozilla/5.0 (X11; Linux x86_64)" }), true);
  assert.equal(isLinuxSerialHost({ platform: "MacIntel" }), false);
});

test("shows Linux guidance for direct serial open failures", () => {
  assert.equal(shouldShowLinuxSerialPermissionHelp({
    isLinux: true,
    stage: "open",
    error: new DOMException("Failed to open serial port.", "NetworkError"),
  }), true);
});

test("shows Linux guidance for bridge permission errors", () => {
  assert.equal(shouldShowLinuxSerialPermissionHelp({
    isLinux: true,
    stage: "bridge",
    error: "serial open failed: Error: Permission denied, cannot open /dev/ttyACM0",
  }), true);
});

test("does not mistake chooser cancellation or bridge availability for device permissions", () => {
  assert.equal(shouldShowLinuxSerialPermissionHelp({
    isLinux: true,
    stage: "request",
    error: new DOMException("No port selected by the user.", "NotFoundError"),
  }), false);
  assert.equal(shouldShowLinuxSerialPermissionHelp({
    isLinux: true,
    stage: "bridge",
    error: "Bridge WebSocket error",
  }), false);
  assert.equal(shouldShowLinuxSerialPermissionHelp({
    isLinux: false,
    stage: "open",
    error: new DOMException("Failed to open serial port.", "NetworkError"),
  }), false);
});
