import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import { LanguageProvider } from "@/lib/i18n";
import { ThemeProvider } from "@/lib/theme";
import { ThemeMenu } from "./ThemeMenu";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

function installMatchMedia(initiallyDark = false) {
  let dark = initiallyDark;
  const listeners = new Set<(event: MediaQueryListEvent) => void>();
  const media = {
    media: "(prefers-color-scheme: dark)",
    get matches() { return dark; },
    onchange: null,
    addEventListener: (_type: string, listener: (event: MediaQueryListEvent) => void) => listeners.add(listener),
    removeEventListener: (_type: string, listener: (event: MediaQueryListEvent) => void) => listeners.delete(listener),
    addListener: vi.fn(),
    removeListener: vi.fn(),
    dispatchEvent: vi.fn(),
  } as MediaQueryList;
  Object.defineProperty(window, "matchMedia", {
    configurable: true,
    value: vi.fn(() => media),
  });
  return {
    setDark(next: boolean) {
      dark = next;
      const event = { matches: next, media: media.media } as MediaQueryListEvent;
      for (const listener of listeners) listener(event);
    },
  };
}

function renderThemeMenu() {
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  act(() => {
    root.render(
      <ThemeProvider>
        <LanguageProvider>
          <ThemeMenu />
        </LanguageProvider>
      </ThemeProvider>
    );
  });
  return {
    host,
    close: () => act(() => {
      root.unmount();
      host.remove();
    }),
  };
}

describe("ThemeMenu", () => {
  afterEach(() => {
    document.body.replaceChildren();
    document.documentElement.className = "";
    document.documentElement.removeAttribute("data-theme");
    document.documentElement.removeAttribute("data-theme-preference");
    document.documentElement.removeAttribute("data-terminal-theme");
    document.documentElement.removeAttribute("data-terminal-font");
    document.documentElement.removeAttribute("data-terminal-font-family");
    document.documentElement.removeAttribute("style");
    localStorage.clear();
    vi.restoreAllMocks();
  });

  it("offers five interface themes, five terminal palettes, and five font presets", () => {
    installMatchMedia(false);
    localStorage.setItem("lang", "en");
    const view = renderThemeMenu();

    act(() => {
      (view.host.querySelector('[aria-haspopup="dialog"]') as HTMLButtonElement).click();
    });
    expect(view.host.querySelectorAll('input[name="interface-theme"]')).toHaveLength(5);
    expect(view.host.querySelectorAll('input[name="terminal-theme"]')).toHaveLength(5);
    expect(view.host.querySelectorAll('select[name="terminal-font"] option')).toHaveLength(5);

    act(() => {
      (view.host.querySelector('input[name="interface-theme"][value="ocean"]') as HTMLInputElement).click();
      (view.host.querySelector('input[name="terminal-theme"][value="phosphor"]') as HTMLInputElement).click();
    });

    expect(document.documentElement.dataset.theme).toBe("ocean");
    expect(document.documentElement.dataset.terminalTheme).toBe("phosphor");
    expect(localStorage.getItem("theme")).toBe("ocean");
    expect(localStorage.getItem("terminal-theme")).toBe("phosphor");
    view.close();
  });

  it("persists a custom terminal font stack and publishes it to live terminals", () => {
    installMatchMedia(false);
    localStorage.setItem("lang", "en");
    const view = renderThemeMenu();
    act(() => {
      (view.host.querySelector('[aria-haspopup="dialog"]') as HTMLButtonElement).click();
    });

    const select = view.host.querySelector('select[name="terminal-font"]') as HTMLSelectElement;
    act(() => {
      select.value = "custom";
      select.dispatchEvent(new Event("change", { bubbles: true }));
    });
    const input = view.host.querySelector('input[name="custom-terminal-font"]') as HTMLInputElement;
    act(() => {
      const setValue = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")?.set;
      setValue?.call(input, '"IBM Plex Mono", ui-monospace, monospace');
      input.dispatchEvent(new Event("input", { bubbles: true }));
    });

    expect(document.documentElement.dataset.terminalFont).toBe("custom");
    expect(document.documentElement.dataset.terminalFontFamily)
      .toBe('"IBM Plex Mono", ui-monospace, monospace');
    expect(localStorage.getItem("terminal-font")).toBe("custom");
    expect(localStorage.getItem("terminal-font-custom"))
      .toBe('"IBM Plex Mono", ui-monospace, monospace');
    view.close();
  });

  it("keeps invalid custom input visible but publishes the safe fallback", () => {
    installMatchMedia(false);
    localStorage.setItem("lang", "en");
    localStorage.setItem("terminal-font", "custom");
    localStorage.setItem("terminal-font-custom", "monospace; color: red");
    const view = renderThemeMenu();

    act(() => {
      (view.host.querySelector('[aria-haspopup="dialog"]') as HTMLButtonElement).click();
    });

    const input = view.host.querySelector('input[name="custom-terminal-font"]') as HTMLInputElement;
    expect(input.value).toBe("monospace; color: red");
    expect(input.getAttribute("aria-invalid")).toBe("true");
    expect(document.documentElement.dataset.terminalFontFamily)
      .toBe("ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace");
    view.close();
  });

  it("updates a system theme when the operating-system preference changes", () => {
    const media = installMatchMedia(false);
    localStorage.setItem("theme", "system");
    localStorage.setItem("lang", "en");
    const view = renderThemeMenu();
    expect(document.documentElement.dataset.theme).toBe("light");

    act(() => media.setDark(true));
    expect(document.documentElement.dataset.theme).toBe("dark");
    expect(document.documentElement.dataset.themePreference).toBe("system");
    view.close();
  });

  it("closes on Escape and returns focus to the trigger", () => {
    installMatchMedia(false);
    localStorage.setItem("lang", "en");
    const view = renderThemeMenu();
    const trigger = view.host.querySelector('[aria-haspopup="dialog"]') as HTMLButtonElement;
    act(() => trigger.click());
    expect(view.host.querySelector('[role="dialog"]')).not.toBeNull();

    act(() => document.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true })));
    expect(view.host.querySelector('[role="dialog"]')).toBeNull();
    expect(document.activeElement).toBe(trigger);
    view.close();
  });
});
