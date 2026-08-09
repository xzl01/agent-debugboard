import { useEffect, useId, useRef, useState } from "react";
import { Check, Monitor, Palette, Terminal, Type } from "lucide-react";
import { useI18n } from "@/lib/i18n";
import {
  MAX_CUSTOM_TERMINAL_FONT_LENGTH,
  TERMINAL_FONTS,
  normalizeCustomTerminalFontFamily,
  type TerminalFont,
} from "@/lib/terminalFont";
import {
  APP_THEMES,
  TERMINAL_THEMES,
  useTheme,
  type TerminalTheme,
  type Theme,
} from "@/lib/theme";
import { cn } from "@/lib/utils";
import { Button } from "./ui";

const THEME_LABELS: Record<Theme, string> = {
  system: "theme.system",
  light: "theme.light",
  dark: "theme.dark",
  ocean: "theme.ocean",
  contrast: "theme.contrast",
};

const TERMINAL_THEME_LABELS: Record<TerminalTheme, string> = {
  adaptive: "theme.terminal.adaptive",
  graphite: "theme.terminal.graphite",
  ocean: "theme.terminal.ocean",
  phosphor: "theme.terminal.phosphor",
  paper: "theme.terminal.paper",
};

const TERMINAL_FONT_LABELS: Record<TerminalFont, string> = {
  system: "theme.font.system",
  maple: "theme.font.maple",
  jetbrains: "theme.font.jetbrains",
  cascadia: "theme.font.cascadia",
  custom: "theme.font.custom",
};

const THEME_SWATCHES: Record<Theme, readonly [string, string, string]> = {
  system: ["#f8fafc", "#0f172a", "#2563eb"],
  light: ["#f8fafc", "#ffffff", "#2563eb"],
  dark: ["#090c12", "#1e293b", "#60a5fa"],
  ocean: ["#03101c", "#0b2134", "#38bdf8"],
  contrast: ["#000000", "#ffffff", "#0070f0"],
};

const TERMINAL_SWATCHES: Record<TerminalTheme, readonly [string, string]> = {
  adaptive: ["rgb(var(--c-terminal))", "rgb(var(--c-terminal-ink))"],
  graphite: ["#0d1117", "#c9d1d9"],
  ocean: ["#02121f", "#38bdf8"],
  phosphor: ["#03130a", "#9ef7b0"],
  paper: ["#f7f5ef", "#24292f"],
};

export function ThemeMenu() {
  const { t } = useI18n();
  const {
    theme,
    setTheme,
    terminalTheme,
    setTerminalTheme,
    terminalFont,
    setTerminalFont,
    customTerminalFont,
    setCustomTerminalFont,
    terminalFontFamily,
  } = useTheme();
  const [open, setOpen] = useState(false);
  const rootRef = useRef<HTMLDivElement>(null);
  const triggerRef = useRef<HTMLButtonElement>(null);
  const panelId = useId();
  const customFontValid = normalizeCustomTerminalFontFamily(customTerminalFont) !== null;

  useEffect(() => {
    if (!open) return undefined;
    const closeOnOutsidePress = (event: PointerEvent) => {
      if (!rootRef.current?.contains(event.target as Node)) setOpen(false);
    };
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key !== "Escape") return;
      setOpen(false);
      triggerRef.current?.focus();
    };
    document.addEventListener("pointerdown", closeOnOutsidePress);
    document.addEventListener("keydown", closeOnEscape);
    return () => {
      document.removeEventListener("pointerdown", closeOnOutsidePress);
      document.removeEventListener("keydown", closeOnEscape);
    };
  }, [open]);

  return (
    <div ref={rootRef} className="relative">
      <Button
        ref={triggerRef}
        variant="ghost"
        className="px-2.5"
        type="button"
        aria-expanded={open}
        aria-haspopup="dialog"
        aria-controls={open ? panelId : undefined}
        title={t("theme.menu")}
        onClick={() => setOpen((value) => !value)}
      >
        <Palette size={16} />
        <span className="hidden sm:inline">{t(THEME_LABELS[theme])}</span>
      </Button>

      {open && (
        <div
          id={panelId}
          role="dialog"
          aria-label={t("theme.menu")}
          className="absolute right-0 top-[calc(100%+0.5rem)] z-30 max-h-[min(36rem,calc(100vh-5rem))] w-[min(20rem,calc(100vw-1rem))] overflow-y-auto overscroll-contain rounded-xl border border-line bg-panel p-2.5 text-ink shadow-xl shadow-black/20"
        >
          <fieldset>
            <legend className="flex items-center gap-2 px-1 pb-2 text-xs font-semibold text-ink">
              <Monitor size={14} className="text-ink-dim" />
              {t("theme.interface")}
            </legend>
            <div className="grid grid-cols-1 gap-1 sm:grid-cols-2">
              {APP_THEMES.map((option) => {
                const checked = theme === option;
                return (
                  <label
                    key={option}
                    className={cn(
                      "flex min-h-10 cursor-pointer items-center gap-2 rounded-lg border px-2.5 py-2 text-xs transition-colors duration-150 focus-within:ring-2 focus-within:ring-brand/50",
                      checked
                        ? "border-brand/60 bg-brand/10 text-ink"
                        : "border-transparent text-ink-dim hover:bg-panel2 hover:text-ink"
                    )}
                  >
                    <input
                      className="sr-only"
                      type="radio"
                      name="interface-theme"
                      value={option}
                      checked={checked}
                      onChange={() => setTheme(option)}
                    />
                    <span className="flex h-5 w-8 shrink-0 overflow-hidden rounded-md border border-line/70" aria-hidden="true">
                      {THEME_SWATCHES[option].map((color) => (
                        <span key={color} className="h-full flex-1" style={{ backgroundColor: color }} />
                      ))}
                    </span>
                    <span className="min-w-0 flex-1 truncate">{t(THEME_LABELS[option])}</span>
                    {checked && <Check size={14} className="shrink-0 text-brand" aria-hidden="true" />}
                  </label>
                );
              })}
            </div>
          </fieldset>

          <div className="my-2 border-t border-line/70" />

          <fieldset>
            <legend className="flex items-center gap-2 px-1 pb-2 text-xs font-semibold text-ink">
              <Terminal size={14} className="text-ink-dim" />
              {t("theme.terminal")}
            </legend>
            <div className="grid grid-cols-1 gap-1 sm:grid-cols-2">
              {TERMINAL_THEMES.map((option) => {
                const checked = terminalTheme === option;
                return (
                  <label
                    key={option}
                    className={cn(
                      "flex min-h-10 cursor-pointer items-center gap-2 rounded-lg border px-2.5 py-2 text-xs transition-colors duration-150 focus-within:ring-2 focus-within:ring-brand/50",
                      checked
                        ? "border-brand/60 bg-brand/10 text-ink"
                        : "border-transparent text-ink-dim hover:bg-panel2 hover:text-ink"
                    )}
                  >
                    <input
                      className="sr-only"
                      type="radio"
                      name="terminal-theme"
                      value={option}
                      checked={checked}
                      onChange={() => setTerminalTheme(option)}
                    />
                    <span
                      className="grid h-5 w-8 shrink-0 place-items-center rounded-md border text-[10px] font-bold leading-none"
                      style={{
                        backgroundColor: TERMINAL_SWATCHES[option][0],
                        borderColor: TERMINAL_SWATCHES[option][1],
                        color: TERMINAL_SWATCHES[option][1],
                      }}
                      aria-hidden="true"
                    >
                      &gt;_
                    </span>
                    <span className="min-w-0 flex-1 truncate">{t(TERMINAL_THEME_LABELS[option])}</span>
                    {checked && <Check size={14} className="shrink-0 text-brand" aria-hidden="true" />}
                  </label>
                );
              })}
            </div>
          </fieldset>

          <div className="my-2 border-t border-line/70" />

          <fieldset>
            <legend className="flex items-center gap-2 px-1 pb-2 text-xs font-semibold text-ink">
              <Type size={14} className="text-ink-dim" />
              {t("theme.font")}
            </legend>
            <label className="block px-1 text-[11px] font-medium text-ink-dim" htmlFor={`${panelId}-terminal-font`}>
              {t("theme.font.preset")}
            </label>
            <select
              id={`${panelId}-terminal-font`}
              name="terminal-font"
              value={terminalFont}
              onChange={(event) => setTerminalFont(event.target.value as TerminalFont)}
              className="mt-1 min-h-10 w-full rounded-lg border border-line bg-panel2 px-2.5 text-xs text-ink outline-none transition-colors focus:border-brand/60 focus:ring-2 focus:ring-brand/30"
            >
              {TERMINAL_FONTS.map((font) => (
                <option key={font} value={font}>{t(TERMINAL_FONT_LABELS[font])}</option>
              ))}
            </select>

            {terminalFont === "custom" && (
              <div className="mt-2 px-1">
                <label className="block text-[11px] font-medium text-ink-dim" htmlFor={`${panelId}-custom-font`}>
                  {t("theme.font.family")}
                </label>
                <input
                  id={`${panelId}-custom-font`}
                  name="custom-terminal-font"
                  value={customTerminalFont}
                  maxLength={MAX_CUSTOM_TERMINAL_FONT_LENGTH}
                  spellCheck={false}
                  autoComplete="off"
                  aria-invalid={!customFontValid}
                  aria-describedby={`${panelId}-font-hint`}
                  placeholder={'"Maple Mono NL", monospace'}
                  onChange={(event) => setCustomTerminalFont(event.target.value)}
                  className={cn(
                    "mt-1 min-h-10 w-full rounded-lg border bg-panel2 px-2.5 font-mono text-xs text-ink outline-none transition-colors focus:ring-2",
                    customFontValid
                      ? "border-line focus:border-brand/60 focus:ring-brand/30"
                      : "border-danger/70 focus:border-danger focus:ring-danger/25"
                  )}
                />
              </div>
            )}

            <div
              className="mt-2 flex min-h-9 items-center justify-between gap-3 rounded-lg border border-line/70 bg-terminal px-2.5 py-1.5 text-terminal-ink"
              style={{ fontFamily: terminalFontFamily }}
            >
              <span className="truncate text-xs">Aa 0O 1l {"{} [] ->"}</span>
              <span className="shrink-0 text-[10px] opacity-70">{t("theme.font.preview")}</span>
            </div>
            <p
              id={`${panelId}-font-hint`}
              className={cn("px-1 pt-1.5 text-[10px] leading-relaxed", customFontValid || terminalFont !== "custom" ? "text-ink-dim" : "text-danger")}
            >
              {terminalFont === "custom" && !customFontValid
                ? t("theme.font.invalid")
                : t("theme.font.hint")}
            </p>
          </fieldset>
        </div>
      )}
    </div>
  );
}
