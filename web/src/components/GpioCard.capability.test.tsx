import { afterEach, describe, expect, it, vi } from "vitest";
import { physicalLayoutFixture } from "@/lib/gpioPhysicalLayout.testFixtures";
import { mount, flush } from "./GpioCard.testUtils";
import { GpioCard } from "./GpioCard";
import { click } from "./GpioPinoutSvg.testUtils";

// Deliberately bypass the child gesture guard: the card must fail closed too.
vi.mock("./GpioPin", async (importOriginal) => ({
  ...await importOriginal<typeof import("./GpioPin")>(),
  GpioPin: ({ cell, onAction }: { cell: { pin: number }; onAction: (pin: number, action: { kind: string }) => void }) =>
    <button data-pin={cell.pin} onClick={() => { onAction(cell.pin, { kind: "outputLow" }); onAction(cell.pin, { kind: "outputHigh" }); }}>Faulty gesture</button>,
}));

afterEach(() => { document.body.replaceChildren(); localStorage.clear(); });

describe("GpioCard independent output-capability gate", () => {
  it("blocks GP29 output even if the gesture component incorrectly dispatches it", async () => {
    const { layout, gpios } = physicalLayoutFixture();
    const onSet = vi.fn();
    const view = mount(<GpioCard physicalLayout={layout} gpios={gpios} onSet={onSet} />);
    click(view.host.querySelector('button[data-pin="29"]')!);
    await flush();
    expect(onSet).not.toHaveBeenCalled();
    view.close();
  });
});
