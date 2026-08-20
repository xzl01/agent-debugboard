import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import { LanguageProvider } from "@/lib/i18n";
import { ThemeProvider } from "@/lib/theme";
import type { BoardSnapshot } from "@/lib/types";
import { StatusBar } from "./StatusBar";
import { Badge } from "./ui";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const emptySnapshot: BoardSnapshot = {
  powerOutputs: [],
  switches: {},
  gpios: [],
  watchdog: {
    supported: false,
    automatic: false,
    healthy: false,
    armed: false,
    timeout_ms: 0,
    bootloader_on_timeout: false,
    failing_service: "",
  },
  monitoring: {
    temperature: { available: false },
    heap: { available: false },
    runtime: { available: false },
    cpu: { available: false },
  },
  adc: [],
};

function render(snapshot: BoardSnapshot): { readonly host: HTMLDivElement; readonly close: () => void } {
  localStorage.setItem("theme", "light");
  localStorage.setItem("lang", "en");
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  act(() => {
    root.render(
      <ThemeProvider>
        <LanguageProvider>
          <StatusBar
            snapshot={snapshot}
            connected
            loading={false}
            auto
            setAuto={vi.fn()}
            live={false}
            setLive={vi.fn()}
            onRefresh={vi.fn()}
          />
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

describe("StatusBar Lighthouse regressions", () => {
  afterEach(() => {
    document.body.replaceChildren();
    localStorage.clear();
  });

  it("keeps the Figma header compact while moving status into a responsive row", () => {
    const view = render(emptySnapshot);
    const details = view.host.querySelector('[data-testid="status-details"]');
    const shell = view.host.querySelector("header > div");

    expect(shell?.classList.contains("min-h-14")).toBe(true);
    expect(shell?.classList.contains("max-w-[1440px]")).toBe(true);
    expect(details?.classList.contains("order-3")).toBe(true);
    expect(details?.classList.contains("md:order-2")).toBe(true);
    view.close();
  });

  it("keeps a fixed USB slot before and after transport data arrives", () => {
    const initial = render(emptySnapshot);
    const placeholder = initial.host.querySelector('[data-testid="status-usb"]');

    expect(placeholder?.textContent?.trim()).toBe("—");
    expect(placeholder?.classList.contains("min-w-20")).toBe(true);
    initial.close();

    const loaded = render({ ...emptySnapshot, usb: "mock-ncm" });
    expect(loaded.host.querySelector('[data-testid="status-usb"]')?.textContent).toContain("mock-ncm");
    loaded.close();
  });

  it("uses restrained status text instead of large semantic badges", () => {
    const view = render({ ...emptySnapshot, usb: "mock-ncm" });
    const online = view.host.querySelector('[data-testid="status-connection"]');
    const usb = view.host.querySelector('[data-testid="status-usb"]');

    expect(online?.classList.contains("text-ok")).toBe(true);
    expect(online?.querySelector("svg")).toBeNull();
    expect(usb?.textContent).toContain("mock-ncm");
    expect(usb?.querySelector("svg")).toBeNull();
    view.close();
  });

  it("keeps every semantic badge tone readable while coloring its icon", () => {
    const host = document.createElement("div");
    document.body.append(host);
    const root = createRoot(host);
    act(() => {
      root.render(
        <>
          <Badge tone="ok" data-testid="tone-ok"><svg /></Badge>
          <Badge tone="warn" data-testid="tone-warn"><svg /></Badge>
          <Badge tone="danger" data-testid="tone-danger"><svg /></Badge>
          <Badge tone="brand" data-testid="tone-brand"><svg /></Badge>
        </>
      );
    });

    for (const tone of ["ok", "warn", "danger", "brand"]) {
      const badge = host.querySelector(`[data-testid="tone-${tone}"]`);
      expect(badge?.classList.contains("text-ink")).toBe(true);
      expect(badge?.classList.contains(`[&>svg]:text-${tone}`)).toBe(true);
    }
    act(() => root.unmount());
    host.remove();
  });
});
