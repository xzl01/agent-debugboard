export const TERMINAL_FONTS = ["system", "maple", "jetbrains", "cascadia", "custom"] as const;

export type TerminalFont = (typeof TERMINAL_FONTS)[number];

export const DEFAULT_TERMINAL_FONT_FAMILY =
  "ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace";

export const TERMINAL_FONT_FAMILIES: Record<Exclude<TerminalFont, "custom">, string> = {
  system: DEFAULT_TERMINAL_FONT_FAMILY,
  maple: '"Maple Mono NL", "Maple Mono", ui-monospace, monospace',
  jetbrains: '"JetBrains Mono", ui-monospace, monospace',
  cascadia: '"Cascadia Mono", "Cascadia Code", ui-monospace, monospace',
};

export const MAX_CUSTOM_TERMINAL_FONT_LENGTH = 160;
const UNSAFE_FONT_CHARACTERS = /[\u0000-\u001f\u007f;{}]/;

export function normalizeCustomTerminalFontFamily(value: string): string | null {
  const normalized = value.trim().replace(/\s+/g, " ");
  if (!normalized || normalized.length > MAX_CUSTOM_TERMINAL_FONT_LENGTH) return null;
  if (UNSAFE_FONT_CHARACTERS.test(normalized)) return null;
  return normalized;
}

export function sanitizeStoredTerminalFontFamily(value: string | null): string {
  if (!value) return "";
  return value
    .slice(0, MAX_CUSTOM_TERMINAL_FONT_LENGTH)
    .replace(/[\u0000-\u001f\u007f]/g, "");
}

export function resolveTerminalFontFamily(font: TerminalFont, customFontFamily: string): string {
  if (font !== "custom") return TERMINAL_FONT_FAMILIES[font];
  return normalizeCustomTerminalFontFamily(customFontFamily) ?? DEFAULT_TERMINAL_FONT_FAMILY;
}

export function terminalFontFamilyFromRoot(root: HTMLElement = document.documentElement): string {
  return normalizeCustomTerminalFontFamily(root.dataset.terminalFontFamily ?? "")
    ?? DEFAULT_TERMINAL_FONT_FAMILY;
}
