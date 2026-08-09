import { afterEach, describe, expect, it } from "vitest";
import { resolveTerminalTheme } from "./terminalTheme";

describe("resolveTerminalTheme", () => {
  afterEach(() => {
    document.documentElement.className = "";
    document.documentElement.removeAttribute("data-terminal-theme");
    document.documentElement.removeAttribute("style");
  });

  it("uses interface tokens for the adaptive terminal palette", () => {
    const root = document.documentElement;
    root.classList.add("dark");
    root.dataset.terminalTheme = "adaptive";
    root.style.setProperty("--c-terminal", "2 12 22");
    root.style.setProperty("--c-terminal-ink", "208 237 250");
    root.style.setProperty("--c-brand", "56 189 248");

    const theme = resolveTerminalTheme(root);
    expect(theme.background).toBe("rgb(2 12 22)");
    expect(theme.foreground).toBe("rgb(208 237 250)");
    expect(theme.cursor).toBe("rgb(56 189 248)");
    expect(theme.selectionBackground).toBe("rgb(56 189 248 / 0.25)");
  });

  it("keeps a fixed palette independent from the interface theme", () => {
    const root = document.documentElement;
    root.dataset.terminalTheme = "paper";
    root.classList.add("dark");

    const theme = resolveTerminalTheme(root);
    expect(theme.background).toBe("#f7f5ef");
    expect(theme.foreground).toBe("#24292f");
    expect(theme.cursor).toBe("#0969da");
  });
});
