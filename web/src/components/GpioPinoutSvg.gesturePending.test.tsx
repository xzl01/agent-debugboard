import { act, useLayoutEffect } from "react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { LanguageProvider } from "@/lib/i18n";
import type { SafeGpio } from "@/lib/types";
import { GpioPinoutSvg } from "./GpioPinoutSvg";
import type { GpioAction } from "./useGpioPinGesture";
import {
  advanceTimers,
  gpio,
  gpioPin,
  holdArc,
  keyDown,
  mount,
  pointerDown,
  pointerUp,
  renderSvg,
  rerenderSvg,
} from "./GpioPinoutSvg.testUtils";

beforeEach(() => {
  vi.useFakeTimers();
});

afterEach(() => {
  vi.useRealTimers();
  document.body.replaceChildren();
  localStorage.clear();
});

// Layout effects run in document order during a commit, before any passive
// effect. Mounting an advancer as a sibling after the pinout therefore fires
// its fake timer or synthetic pointerup in the exact window where the pin's
// blocked ref is already synced but the passive pending-cancellation effect
// has not run yet — the only interleaving where the in-callback and
// in-handler gate rechecks are observable.
function TimerAdvancer({ ms }: { readonly ms: number }) {
  useLayoutEffect(() => {
    vi.advanceTimersByTime(ms);
  }, [ms]);
  return null;
}

function PointerUpAdvancer({ pin }: { readonly pin: number }) {
  useLayoutEffect(() => {
    const element = document.querySelector(`[role="button"][aria-label*="GP${pin},"]`);
    if (!element) throw new Error(`GPIO pin GP${pin} not found`);
    element.dispatchEvent(
      new PointerEvent("pointerup", { bubbles: true, cancelable: true, pointerId: 1 })
    );
  }, [pin]);
  return null;
}

function PendingProbe({
  gpios,
  onGpioAction,
  pending = false,
  advanceMs = null,
  releasePin = null,
}: {
  readonly gpios: SafeGpio[];
  readonly onGpioAction: (pin: number, action: GpioAction) => void;
  readonly pending?: boolean;
  readonly advanceMs?: number | null;
  readonly releasePin?: number | null;
}) {
  return (
    <>
      <GpioPinoutSvg
        variant="gpio"
        gpios={gpios}
        onGpioAction={onGpioAction}
        gpioPendingPin={pending ? 10 : null}
      />
      {advanceMs !== null && <TimerAdvancer ms={advanceMs} />}
      {releasePin !== null && <PointerUpAdvancer pin={releasePin} />}
    </>
  );
}

describe("GpioPinoutSvg gpio gesture pending gate", () => {
  it("cancels an in-progress gesture when a request becomes pending", () => {
    const onGpioAction = vi.fn();
    const gpios = [gpio(10)];
    const view = renderSvg({ variant: "gpio", gpios, onGpioAction });
    const pin = gpioPin(view.host, 10);

    pointerDown(pin);
    advanceTimers(300);
    rerenderSvg(view, { variant: "gpio", gpios, onGpioAction, gpioPendingPin: 10 });
    advanceTimers(2000);

    expect(onGpioAction).not.toHaveBeenCalled();
    expect(holdArc(pin)).toBeNull();
    view.close();
  });

  it("keeps gestures inert while a request is pending", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({
      variant: "gpio",
      gpios: [gpio(10)],
      onGpioAction,
      gpioPendingPin: 10,
    });
    const pin = gpioPin(view.host, 10);

    pointerDown(pin);
    pointerUp(pin);
    advanceTimers(2000);
    keyDown(pin, "1");
    keyDown(pin, "Enter");
    keyDown(pin, "i");

    expect(onGpioAction).not.toHaveBeenCalled();
    expect(holdArc(pin)).toBeNull();
    view.close();
  });

  it("rechecks the pending gate inside a completing delayed-LOW timer callback", () => {
    const onGpioAction = vi.fn();
    const view = mount(<PendingProbe gpios={[gpio(10)]} onGpioAction={onGpioAction} />);

    pointerDown(gpioPin(view.host, 10));
    pointerUp(gpioPin(view.host, 10));

    act(() => {
      view.root.render(
        <LanguageProvider>
          <PendingProbe gpios={[gpio(10)]} onGpioAction={onGpioAction} pending advanceMs={220} />
        </LanguageProvider>
      );
    });

    expect(onGpioAction).not.toHaveBeenCalled();
    view.close();
  });

  it("rechecks the pending gate inside a completing hold-HIGH timer callback", () => {
    const onGpioAction = vi.fn();
    const view = mount(<PendingProbe gpios={[gpio(10)]} onGpioAction={onGpioAction} />);

    pointerDown(gpioPin(view.host, 10));

    act(() => {
      view.root.render(
        <LanguageProvider>
          <PendingProbe gpios={[gpio(10)]} onGpioAction={onGpioAction} pending advanceMs={600} />
        </LanguageProvider>
      );
    });

    expect(onGpioAction).not.toHaveBeenCalled();
    view.close();
  });

  it("rechecks the pending gate on the second short release before requesting input", () => {
    const onGpioAction = vi.fn();
    const view = mount(<PendingProbe gpios={[gpio(10)]} onGpioAction={onGpioAction} />);
    const pin = gpioPin(view.host, 10);

    pointerDown(pin);
    pointerUp(pin);
    pointerDown(pin);

    act(() => {
      view.root.render(
        <LanguageProvider>
          <PendingProbe gpios={[gpio(10)]} onGpioAction={onGpioAction} pending releasePin={10} />
        </LanguageProvider>
      );
    });
    advanceTimers(2000);

    expect(onGpioAction).not.toHaveBeenCalled();
    view.close();
  });
});
