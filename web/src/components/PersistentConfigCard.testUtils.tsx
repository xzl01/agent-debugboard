import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { vi } from "vitest";
import {
  usePersistentConfig,
  type UsePersistentConfig,
  type UsePersistentConfigOptions,
} from "@/hooks/usePersistentConfig";
import { LanguageProvider, type Lang } from "@/lib/i18n";
import type {
  PersistentConfig,
  PersistentConfigItem,
} from "@/lib/persistentConfig";
import { PersistentConfigCard } from "./PersistentConfigCard";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

export function powerItem(
  id: string,
  options: {
    readonly selected?: boolean;
    readonly risk?: "safe" | "confirmation_required" | "unknown";
    readonly applyState?: PersistentConfigItem["applyState"];
  } = {}
): Extract<PersistentConfigItem, { kind: "power" }> {
  return {
    id,
    kind: "power",
    current: { kind: "power", state: "on" },
    saved: { kind: "power", state: "off" },
    selected: options.selected ?? false,
    risk: options.risk ?? "safe",
    applyState: options.applyState ?? "not_saved",
  };
}

export function switchItem(
  id: string,
  risk: "safe" | "confirmation_required" | "unknown" = "safe",
  applyState: PersistentConfigItem["applyState"] = "not_saved"
): Extract<PersistentConfigItem, { kind: "switch" }> {
  return {
    id,
    kind: "switch",
    current: { kind: "switch", route: "firmware-current" },
    saved: { kind: "switch", route: "firmware-saved" },
    selected: false,
    risk,
    applyState,
  };
}

export function gpioItem(id: string): Extract<PersistentConfigItem, { kind: "gpio" }> {
  return {
    id,
    kind: "gpio",
    current: { kind: "gpio", direction: "input", value: 0 },
    saved: { kind: "gpio", direction: "output", value: 1 },
    selected: false,
    risk: "safe",
    applyState: "applied",
  };
}

export function config(
  items: readonly PersistentConfigItem[],
  options: {
    readonly available?: boolean;
    readonly pending?: number;
    readonly present?: boolean;
  } = {}
): PersistentConfig {
  return {
    backend: { available: options.available ?? true, reason: "fixture_reason" },
    snapshot: { present: options.present ?? true, version: options.present === false ? null : 7 },
    pending: options.pending ?? 0,
    items,
  };
}

export function state(overrides: Partial<UsePersistentConfig> = {}): UsePersistentConfig {
  return {
    config: config([]),
    error: null,
    loading: false,
    busy: null,
    supported: true,
    refresh: vi.fn().mockResolvedValue(undefined),
    save: vi.fn().mockResolvedValue(undefined),
    clear: vi.fn().mockResolvedValue(undefined),
    ...overrides,
  };
}

export type CardView = {
  readonly host: HTMLDivElement;
  readonly update: (next: UsePersistentConfig, connected?: boolean) => void;
  readonly close: () => void;
};

export type HookedCardView = {
  readonly host: HTMLDivElement;
  readonly update: (next: UsePersistentConfigOptions) => void;
  readonly close: () => void;
};

function render(root: Root, next: UsePersistentConfig, connected: boolean): void {
  act(() => {
    root.render(
      <LanguageProvider>
        <PersistentConfigCard state={next} connected={connected} />
      </LanguageProvider>
    );
  });
}

export function mount(
  initial: UsePersistentConfig,
  options: { readonly connected?: boolean; readonly lang?: Lang } = {}
): CardView {
  localStorage.setItem("lang", options.lang ?? "en");
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  render(root, initial, options.connected ?? true);
  return {
    host,
    update: (next, connected = options.connected ?? true) => render(root, next, connected),
    close: () => act(() => {
      root.unmount();
      host.remove();
    }),
  };
}

function HookedCard(options: UsePersistentConfigOptions) {
  const state = usePersistentConfig(options);
  return <PersistentConfigCard state={state} connected={options.connected} />;
}

export function mountHooked(initial: UsePersistentConfigOptions): HookedCardView {
  localStorage.setItem("lang", "en");
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  const update = (next: UsePersistentConfigOptions) => act(() => {
    root.render(
      <LanguageProvider>
        <HookedCard {...next} />
      </LanguageProvider>
    );
  });
  update(initial);
  return {
    host,
    update,
    close: () => act(() => {
      root.unmount();
      host.remove();
    }),
  };
}

export function button(host: HTMLElement, name: string): HTMLButtonElement {
  const found = [...host.querySelectorAll("button")].find(
    (candidate) => candidate.textContent?.trim() === name
  );
  if (!found) throw new TypeError(`Button not found: ${name}`);
  return found;
}

export function configRow(host: HTMLElement, id: string): HTMLButtonElement {
  const found = [...host.querySelectorAll<HTMLButtonElement>('button[role="checkbox"]')].find(
    (candidate) => candidate.value === id
  );
  if (!found) throw new TypeError(`Config row not found: ${id}`);
  return found;
}

export function click(element: HTMLElement): void {
  act(() => element.click());
}

export function press(key: string, shiftKey = false): void {
  act(() => window.dispatchEvent(new KeyboardEvent("keydown", { key, shiftKey })));
}

export async function flush(): Promise<void> {
  await act(async () => Promise.resolve());
}

export function deferred<T>(): {
  readonly promise: Promise<T>;
  readonly resolve: (value: T) => void;
} {
  let resolve: (value: T) => void = () => undefined;
  const promise = new Promise<T>((done) => {
    resolve = done;
  });
  return { promise, resolve };
}
