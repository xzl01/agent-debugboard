import type { ITheme } from "@xterm/xterm";
import type { TerminalTheme } from "@/lib/theme";

const DARK_ANSI: Pick<ITheme,
  "black" | "red" | "green" | "yellow" | "blue" | "magenta" | "cyan" | "white" |
  "brightBlack" | "brightRed" | "brightGreen" | "brightYellow" | "brightBlue" |
  "brightMagenta" | "brightCyan" | "brightWhite"
> = {
  black: "#0b1220",
  red: "#ef4444",
  green: "#22c55e",
  yellow: "#eab308",
  blue: "#3b82f6",
  magenta: "#a855f7",
  cyan: "#06b6d4",
  white: "#cbd5e1",
  brightBlack: "#64748b",
  brightRed: "#fb7185",
  brightGreen: "#4ade80",
  brightYellow: "#fde047",
  brightBlue: "#60a5fa",
  brightMagenta: "#c084fc",
  brightCyan: "#67e8f9",
  brightWhite: "#ffffff",
};

const LIGHT_ANSI: typeof DARK_ANSI = {
  black: "#172033",
  red: "#b91c1c",
  green: "#15803d",
  yellow: "#a16207",
  blue: "#1d4ed8",
  magenta: "#7e22ce",
  cyan: "#0e7490",
  white: "#e2e8f0",
  brightBlack: "#64748b",
  brightRed: "#dc2626",
  brightGreen: "#16a34a",
  brightYellow: "#ca8a04",
  brightBlue: "#2563eb",
  brightMagenta: "#9333ea",
  brightCyan: "#0891b2",
  brightWhite: "#ffffff",
};

const FIXED_TERMINAL_THEMES: Record<Exclude<TerminalTheme, "adaptive">, ITheme> = {
  graphite: {
    background: "#0d1117",
    foreground: "#c9d1d9",
    cursor: "#58a6ff",
    cursorAccent: "#0d1117",
    selectionBackground: "#58a6ff40",
    ...DARK_ANSI,
  },
  ocean: {
    background: "#02121f",
    foreground: "#d0edfa",
    cursor: "#38bdf8",
    cursorAccent: "#02121f",
    selectionBackground: "#38bdf840",
    ...DARK_ANSI,
    blue: "#38bdf8",
    brightBlue: "#7dd3fc",
    cyan: "#22d3ee",
    brightCyan: "#a5f3fc",
  },
  phosphor: {
    background: "#03130a",
    foreground: "#9ef7b0",
    cursor: "#58ff84",
    cursorAccent: "#03130a",
    selectionBackground: "#58ff8438",
    ...DARK_ANSI,
    black: "#03130a",
    green: "#4ade80",
    brightGreen: "#86efac",
    white: "#9ef7b0",
    brightWhite: "#d1fad9",
  },
  paper: {
    background: "#f7f5ef",
    foreground: "#24292f",
    cursor: "#0969da",
    cursorAccent: "#f7f5ef",
    selectionBackground: "#0969da2e",
    ...LIGHT_ANSI,
  },
};

function cssToken(styles: CSSStyleDeclaration, name: string, fallback: string): string {
  const value = styles.getPropertyValue(name).trim();
  return value ? `rgb(${value})` : fallback;
}

export function resolveTerminalTheme(root: HTMLElement = document.documentElement): ITheme {
  const selected = root.dataset.terminalTheme as TerminalTheme | undefined;
  if (selected && selected !== "adaptive" && selected in FIXED_TERMINAL_THEMES) {
    return FIXED_TERMINAL_THEMES[selected];
  }

  const styles = window.getComputedStyle(root);
  const dark = root.classList.contains("dark");
  const background = cssToken(styles, "--c-terminal", dark ? "#07090c" : "#ffffff");
  const foreground = cssToken(styles, "--c-terminal-ink", dark ? "#e2e8f0" : "#0f172a");
  const cursor = cssToken(styles, "--c-brand", dark ? "#60a5fa" : "#2563eb");
  return {
    background,
    foreground,
    cursor,
    cursorAccent: background,
    selectionBackground: cursor.startsWith("rgb(")
      ? cursor.replace(")", " / 0.25)")
      : `${cursor}40`,
    ...(dark ? DARK_ANSI : LIGHT_ANSI),
  };
}
