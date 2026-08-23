import { act, type ReactElement } from "react";
import { createRoot, type Root } from "react-dom/client";
import { LanguageProvider } from "@/lib/i18n";
import type { SafeGpio } from "@/lib/types";
import { GpioCard } from "./GpioCard";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

export type OnSet = (identifier: string, direction: "input" | "output", value?: number) => Promise<void>;

export function gpio(pin: number, overrides: Partial<SafeGpio> = {}): SafeGpio {
  return {
    name: `sig${pin}`,
    pin,
    note: `note ${pin}`,
    value: 0,
    direction: "input",
    layoutGroup: "J16",
    layoutLabel: `GP${pin}`,
    layoutRow: pin - 10,
    layoutColumn: 0,
    ...overrides,
  };
}

export interface View {
  readonly host: HTMLDivElement;
  readonly root: Root;
  readonly close: () => void;
}

export function mount(element: ReactElement): View {
  localStorage.setItem("lang", "en");
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  act(() => {
    root.render(<LanguageProvider>{element}</LanguageProvider>);
  });
  return {
    host,
    root,
    close: () => {
      act(() => root.unmount());
      host.remove();
    },
  };
}

export function renderCard(gpios: SafeGpio[], onSet: OnSet): View {
  return mount(<GpioCard gpios={gpios} onSet={onSet} />);
}

export function rerenderCard(view: View, gpios: SafeGpio[], onSet: OnSet): void {
  act(() => {
    view.root.render(
      <LanguageProvider>
        <GpioCard gpios={gpios} onSet={onSet} />
      </LanguageProvider>
    );
  });
}

export function pinButton(host: HTMLElement, pin: number): SVGGElement {
  const element = host.querySelector<SVGGElement>(`[role="button"][aria-label*="GP${pin},"]`);
  if (!element) throw new Error(`GPIO pin GP${pin} not found`);
  return element;
}

export async function flush(): Promise<void> {
  await act(async () => {
    await Promise.resolve();
  });
}
