import assert from "node:assert/strict";
import test from "node:test";
import {
  DEFAULT_TERMINAL_FONT_FAMILY,
  MAX_CUSTOM_TERMINAL_FONT_LENGTH,
  normalizeCustomTerminalFontFamily,
  resolveTerminalFontFamily,
  sanitizeStoredTerminalFontFamily,
} from "./terminalFont.ts";

test("normalizes a bounded custom CSS font-family stack", () => {
  assert.equal(
    normalizeCustomTerminalFontFamily('  "IBM Plex Mono",   ui-monospace, monospace  '),
    '"IBM Plex Mono", ui-monospace, monospace',
  );
  assert.equal(normalizeCustomTerminalFontFamily("monospace; color: red"), null);
  assert.equal(normalizeCustomTerminalFontFamily("font{family}"), null);
  assert.equal(normalizeCustomTerminalFontFamily("\n\t"), null);
});

test("resolves presets and safely falls back from an invalid custom value", () => {
  assert.match(resolveTerminalFontFamily("maple", ""), /Maple Mono/);
  assert.equal(resolveTerminalFontFamily("custom", "bad;value"), DEFAULT_TERMINAL_FONT_FAMILY);
  assert.equal(resolveTerminalFontFamily("custom", "Iosevka, monospace"), "Iosevka, monospace");
});

test("bounds persisted custom font input before rendering it", () => {
  assert.equal(sanitizeStoredTerminalFontFamily("A\nB\tC"), "ABC");
  assert.equal(
    sanitizeStoredTerminalFontFamily("x".repeat(MAX_CUSTOM_TERMINAL_FONT_LENGTH + 40)).length,
    MAX_CUSTOM_TERMINAL_FONT_LENGTH,
  );
});
