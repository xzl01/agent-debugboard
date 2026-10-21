import type { SwitchState } from "./types.ts";
import { x12GpioStatusFixture } from "./gpioLayout.testFixtures.ts";

/** Test-only x1.2 control descriptors; never a runtime fallback catalog. */
export function x12SwitchesFixture(): SwitchState {
  return {
    sd: { route: "target", routes: ["target", "usb-reader"], requires_confirm: false },
    tf_wp: { route: "writable", routes: ["writable", "protected"], requires_confirm: false },
  };
}

export function x12ControlStatusFixture() {
  return { ...x12GpioStatusFixture(), switches: x12SwitchesFixture() };
}
