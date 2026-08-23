import { afterEach, describe, expect, it, vi } from "vitest";
import {
  LA_FILL_SELECTED,
  LA_FILL_TRIGGER,
  LA_FILL_UNSELECTED,
  click,
  contextMenu,
  gpio,
  keyDown,
  laPin,
  renderSvg,
  visibleCircle,
} from "./GpioPinoutSvg.testUtils";

afterEach(() => {
  document.body.replaceChildren();
  localStorage.clear();
});

describe("GpioPinoutSvg logic-analyzer default variant", () => {
  it("keeps selection/trigger colors, img role, and pointer-only pins unchanged", () => {
    const onTogglePin = vi.fn();
    const onSetTriggerPin = vi.fn();
    const view = renderSvg({
      gpios: [gpio(10), gpio(11), gpio(12)],
      selectedPins: [10, 11],
      triggerPin: 11,
      triggerActive: true,
      onTogglePin,
      onSetTriggerPin,
    });

    const svg = view.host.querySelector("svg");
    expect(svg?.getAttribute("role")).toBe("img");
    expect(view.host.querySelector('[role="button"]')).toBeNull();
    expect(view.host.querySelector('[class*="group-focus-visible"]')).toBeNull();
    expect(laPin(view.host, "GP10").getAttribute("tabindex")).toBeNull();
    expect(laPin(view.host, "GP10").getAttribute("class")).not.toContain("group");
    expect(laPin(view.host, "GP10").getAttribute("class")).not.toContain("focus-visible:outline-none");
    expect(view.host.textContent).toContain("Not selected");
    expect(view.host.textContent).toContain("Selected for capture");
    expect(view.host.textContent).toContain("Trigger pin");

    expect(visibleCircle(laPin(view.host, "GP10")).getAttribute("fill")).toBe(LA_FILL_SELECTED);
    expect(visibleCircle(laPin(view.host, "GP11")).getAttribute("fill")).toBe(LA_FILL_TRIGGER);
    expect(visibleCircle(laPin(view.host, "GP12")).getAttribute("fill")).toBe(LA_FILL_UNSELECTED);

    click(laPin(view.host, "GP10"));
    expect(onSetTriggerPin).toHaveBeenLastCalledWith(10);
    click(laPin(view.host, "GP11"));
    expect(onSetTriggerPin).toHaveBeenLastCalledWith(null);
    contextMenu(laPin(view.host, "GP12"));
    expect(onTogglePin).toHaveBeenCalledWith(12);

    keyDown(laPin(view.host, "GP10"), "Enter");
    expect(onSetTriggerPin).toHaveBeenCalledTimes(2);
    view.close();
  });
});
