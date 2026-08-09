import {
  createContext,
  useContext,
  useEffect,
  useMemo,
  useState,
  type ReactNode,
} from "react";
import {
  TERMINAL_FONTS,
  resolveTerminalFontFamily,
  sanitizeStoredTerminalFontFamily,
  type TerminalFont,
} from "@/lib/terminalFont";

export const APP_THEMES = ["system", "light", "dark", "ocean", "contrast"] as const;
export const TERMINAL_THEMES = ["adaptive", "graphite", "ocean", "phosphor", "paper"] as const;

export type Theme = (typeof APP_THEMES)[number];
export type ResolvedTheme = Exclude<Theme, "system">;
export type TerminalTheme = (typeof TERMINAL_THEMES)[number];

interface ThemeContextValue {
  theme: Theme;
  resolvedTheme: ResolvedTheme;
  setTheme: (theme: Theme) => void;
  terminalTheme: TerminalTheme;
  setTerminalTheme: (theme: TerminalTheme) => void;
  terminalFont: TerminalFont;
  setTerminalFont: (font: TerminalFont) => void;
  customTerminalFont: string;
  setCustomTerminalFont: (fontFamily: string) => void;
  terminalFontFamily: string;
  toggle: () => void;
}

const ThemeContext = createContext<ThemeContextValue | null>(null);
const THEME_STORAGE_KEY = "theme";
const TERMINAL_THEME_STORAGE_KEY = "terminal-theme";
const TERMINAL_FONT_STORAGE_KEY = "terminal-font";
const CUSTOM_TERMINAL_FONT_STORAGE_KEY = "terminal-font-custom";

function includesValue<T extends string>(values: readonly T[], value: unknown): value is T {
  return typeof value === "string" && values.includes(value as T);
}

function readStoredChoice<T extends string>(
  key: string,
  values: readonly T[],
  fallback: T,
): T {
  if (typeof localStorage === "undefined") return fallback;
  const saved = localStorage.getItem(key);
  return includesValue(values, saved) ? saved : fallback;
}

function systemTheme(): ResolvedTheme {
  if (typeof window !== "undefined" && window.matchMedia) {
    return window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  }
  return "dark";
}

function getInitialTheme(): Theme {
  return readStoredChoice(THEME_STORAGE_KEY, APP_THEMES, "system");
}

function getInitialTerminalTheme(): TerminalTheme {
  return readStoredChoice(TERMINAL_THEME_STORAGE_KEY, TERMINAL_THEMES, "adaptive");
}

function getInitialTerminalFont(): TerminalFont {
  return readStoredChoice(TERMINAL_FONT_STORAGE_KEY, TERMINAL_FONTS, "system");
}

function getInitialCustomTerminalFont(): string {
  if (typeof localStorage === "undefined") return "";
  return sanitizeStoredTerminalFontFamily(localStorage.getItem(CUSTOM_TERMINAL_FONT_STORAGE_KEY));
}

export function ThemeProvider({ children }: { children: ReactNode }) {
  const [theme, setTheme] = useState<Theme>(getInitialTheme);
  const [terminalTheme, setTerminalTheme] = useState<TerminalTheme>(getInitialTerminalTheme);
  const [terminalFont, setTerminalFont] = useState<TerminalFont>(getInitialTerminalFont);
  const [customTerminalFont, setCustomTerminalFont] = useState(getInitialCustomTerminalFont);
  const [systemPreference, setSystemPreference] = useState<ResolvedTheme>(systemTheme);

  useEffect(() => {
    if (typeof window === "undefined" || !window.matchMedia) return undefined;
    const media = window.matchMedia("(prefers-color-scheme: dark)");
    const update = (event?: MediaQueryListEvent) => {
      setSystemPreference((event?.matches ?? media.matches) ? "dark" : "light");
    };
    update();
    if (media.addEventListener) {
      media.addEventListener("change", update);
      return () => media.removeEventListener("change", update);
    }
    media.addListener?.(update);
    return () => media.removeListener?.(update);
  }, []);

  const resolvedTheme: ResolvedTheme = theme === "system" ? systemPreference : theme;
  const terminalFontFamily = resolveTerminalFontFamily(terminalFont, customTerminalFont);

  useEffect(() => {
    const root = document.documentElement;
    const darkSurface = resolvedTheme !== "light";
    root.classList.toggle("dark", darkSurface);
    root.dataset.theme = resolvedTheme;
    root.dataset.themePreference = theme;
    root.dataset.terminalTheme = terminalTheme;
    root.dataset.terminalFont = terminalFont;
    root.dataset.terminalFontFamily = terminalFontFamily;
    root.style.colorScheme = darkSurface ? "dark" : "light";
    localStorage.setItem(THEME_STORAGE_KEY, theme);
    localStorage.setItem(TERMINAL_THEME_STORAGE_KEY, terminalTheme);
    localStorage.setItem(TERMINAL_FONT_STORAGE_KEY, terminalFont);
    localStorage.setItem(CUSTOM_TERMINAL_FONT_STORAGE_KEY, customTerminalFont);
  }, [customTerminalFont, resolvedTheme, terminalFont, terminalFontFamily, terminalTheme, theme]);

  const value = useMemo<ThemeContextValue>(() => ({
    theme,
    resolvedTheme,
    setTheme,
    terminalTheme,
    setTerminalTheme,
    terminalFont,
    setTerminalFont,
    customTerminalFont,
    setCustomTerminalFont,
    terminalFontFamily,
    toggle: () => setTheme(resolvedTheme === "dark" ? "light" : "dark"),
  }), [customTerminalFont, resolvedTheme, terminalFont, terminalFontFamily, terminalTheme, theme]);

  return <ThemeContext.Provider value={value}>{children}</ThemeContext.Provider>;
}

export function useTheme(): ThemeContextValue {
  const ctx = useContext(ThemeContext);
  if (!ctx) throw new Error("useTheme must be used within ThemeProvider");
  return ctx;
}
