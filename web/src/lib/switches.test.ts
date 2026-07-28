import { describe, it } from "node:test";
import assert from "node:assert/strict";
import {
  parseSwitches,
  switchDescLabel,
  switchNameLabel,
  switchRouteLabel,
} from "./switches.ts";

describe("parseSwitches", () => {
  it("parses a dynamic switch map with routes and requires_confirm", () => {
    const parsed = parseSwitches({
      tf_wp: { route: "writable", routes: ["writable", "protected"], requires_confirm: false },
      sd: { route: "target", routes: ["target", "usb-reader"] },
      vin: { route: "3.3v", routes: ["1.8v", "3.3v"], requires_confirm: true },
    });
    assert.deepEqual(parsed, {
      tf_wp: { route: "writable", routes: ["writable", "protected"], requires_confirm: false },
      sd: { route: "target", routes: ["target", "usb-reader"], requires_confirm: undefined },
      vin: { route: "3.3v", routes: ["1.8v", "3.3v"], requires_confirm: true },
    });
  });

  it("parses legacy firmware JSON without routes arrays", () => {
    const parsed = parseSwitches({
      sd: { route: "usb-reader" },
      usb: { route: "pc" },
    });
    assert.deepEqual(parsed, {
      sd: { route: "usb-reader", routes: undefined, requires_confirm: undefined },
      usb: { route: "pc", routes: undefined, requires_confirm: undefined },
    });
  });

  it("accepts switch names the UI has never seen", () => {
    const parsed = parseSwitches({
      aux_mux: { route: "lane-a", routes: ["lane-a", "lane-b"] },
    });
    assert.equal(parsed.aux_mux.route, "lane-a");
    assert.deepEqual(parsed.aux_mux.routes, ["lane-a", "lane-b"]);
  });

  it("returns previous state when the payload is not an object", () => {
    const prev = { sd: { route: "target" } };
    assert.equal(parseSwitches(undefined, prev), prev);
    assert.equal(parseSwitches(null, prev), prev);
    assert.equal(parseSwitches("switches", prev), prev);
    assert.deepEqual(parseSwitches(undefined), {});
  });

  it("ignores non-object switch entries and non-string routes", () => {
    const parsed = parseSwitches({
      sd: "target",
      usb: { route: 5, routes: ["pc", 7] },
    });
    assert.deepEqual(parsed, {
      usb: { route: "", routes: ["pc"], requires_confirm: undefined },
    });
  });

  it("merges websocket payloads with previous state per switch and field", () => {
    const prev = {
      sd: { route: "target", routes: ["target", "usb-reader"] },
      tf_wp: { route: "writable", routes: ["writable", "protected"] },
    };
    const merged = parseSwitches(
      { sd: { route: "usb-reader" } },
      prev
    );
    assert.deepEqual(merged, {
      sd: { route: "usb-reader", routes: ["target", "usb-reader"], requires_confirm: undefined },
      tf_wp: { route: "writable", routes: ["writable", "protected"] },
    });
  });
});

describe("switch labels", () => {
  const t = (key: string): string =>
    ({
      "switch.name.sd": "SD / TF card",
      "switch.desc.sd": "Route the microSD between targets",
      "switch.route.writable": "Writable",
    })[key] ?? key;

  it("translates known switch names, descriptions, and routes", () => {
    assert.equal(switchNameLabel(t, "sd"), "SD / TF card");
    assert.equal(switchDescLabel(t, "sd"), "Route the microSD between targets");
    assert.equal(switchRouteLabel(t, "writable"), "Writable");
  });

  it("falls back to raw firmware strings for unknown switches and routes", () => {
    assert.equal(switchNameLabel(t, "aux_mux"), "aux_mux");
    assert.equal(switchRouteLabel(t, "lane-b"), "lane-b");
  });

  it("returns an empty description for unknown switches", () => {
    assert.equal(switchDescLabel(t, "aux_mux"), "");
  });
});
