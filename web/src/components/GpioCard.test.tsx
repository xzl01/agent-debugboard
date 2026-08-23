import { act } from "react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { useI18n } from "@/lib/i18n";
import { GpioCard } from "./GpioCard";
import { gpio, mount, renderCard } from "./GpioCard.testUtils";

afterEach(() => {
  document.body.replaceChildren();
  localStorage.clear();
});

describe("GpioCard compact pinout", () => {
  it("keeps the output-count badge", () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard(
      [
        gpio(10, { direction: "input" }),
        gpio(11, { direction: "output", value: 1 }),
        gpio(12, { direction: "output", value: 0 }),
      ],
      onSet
    );

    expect(view.host.textContent).toContain("outputs: 2/3");
    view.close();
  });

  it("renders the gesture hint as four inline-block phrase chunks", () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10)], onSet);

    const hint = view.host.querySelector("p[id]");
    const chunks = Array.from(hint?.querySelectorAll("span.inline-block") ?? []);
    expect(chunks).toHaveLength(4);
    expect(chunks[0]?.textContent).toBe("Tap: output low");
    expect(chunks[1]?.textContent).toBe("Double-tap: input");
    expect(chunks[2]?.textContent).toBe("Hold: output high");
    expect(chunks[3]?.textContent).toBe("Keys: Enter/Space/0 low, 1 high, I input");
    expect(hint?.textContent).not.toContain("Click a pin to select it");
    view.close();
  });

  it("keeps each Chinese hint phrase wrappable only as a unit", () => {
    let switchLang: ((lang: "en" | "zh") => void) | undefined;
    function LangProbe(): null {
      const { setLang } = useI18n();
      switchLang = setLang;
      return null;
    }
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = mount(
      <>
        <LangProbe />
        <GpioCard gpios={[gpio(10)]} onSet={onSet} />
      </>
    );

    act(() => {
      switchLang?.("zh");
    });

    const chunks = Array.from(
      view.host.querySelector("p[id]")?.querySelectorAll("span.inline-block") ?? []
    );
    expect(chunks).toHaveLength(4);
    expect(chunks[0]?.textContent).toBe("轻点：输出低电平");
    expect(chunks[1]?.textContent).toBe("双击：设为输入");
    expect(chunks[2]?.textContent).toBe("长按：输出高电平");
    expect(chunks[3]?.textContent).toBe("按键：Enter/Space/0 低电平，1 高电平，I 输入");
    view.close();
  });

  it("renders no selection details and no action button row", () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10)], onSet);

    expect(view.host.textContent).not.toContain("No pin selected.");
    expect(view.host.textContent).not.toContain("Current direction");
    expect(view.host.querySelector("button")).toBeNull();
    expect(view.host.querySelector('[aria-pressed]')).toBeNull();
    view.close();
  });

  it("constrains the pinout section to the compact left-pane width", () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10)], onSet);

    const section = view.host.querySelector("section.min-w-0");
    expect(section?.classList.contains("w-full")).toBe(true);
    expect(section?.classList.contains("max-w-[300px]")).toBe(true);
    view.close();
  });

  it("renders the empty state when no safe GPIOs are reported", () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([], onSet);

    expect(view.host.textContent).toContain("No safe GPIOs reported.");
    view.close();
  });
});
