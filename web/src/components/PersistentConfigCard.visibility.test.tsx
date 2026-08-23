import { afterEach, describe, expect, it } from "vitest";
import { PersistentConfigApiError } from "@/lib/persistentConfig";
import {
  click,
  config,
  configRow,
  gpioItem,
  mount,
  powerItem,
  state,
  switchItem,
  type CardView,
} from "./PersistentConfigCard.testUtils";

type GroupKind = "power" | "switch" | "gpio";

function groupDetails(host: HTMLElement, kind: GroupKind): HTMLDetailsElement {
  const heading = host.querySelector(`#persistent-config-group-${kind}`);
  if (!heading) throw new TypeError(`Group heading not found: ${kind}`);
  const details = heading.closest("details");
  if (!details) throw new TypeError(`Group details not found: ${kind}`);
  return details;
}

const PARENT_DISCLOSURE_LABELS = [
  "Expand saved configuration",
  "Collapse saved configuration",
  "展开已保存配置",
  "折叠已保存配置",
];

function parentDisclosureButton(host: HTMLElement): HTMLButtonElement | undefined {
  return [...host.querySelectorAll("button")].find((candidate) => {
    const label = candidate.getAttribute("aria-label");
    return label !== null && PARENT_DISCLOSURE_LABELS.includes(label);
  });
}

function threeKindFixture() {
  return state({
    config: config([
      powerItem("power.5v_out"),
      switchItem("switch.tf"),
      gpioItem("gpio.gp7"),
    ]),
  });
}

let view: CardView | null = null;
afterEach(() => {
  view?.close();
  view = null;
  localStorage.clear();
});

describe("PersistentConfigCard visibility", () => {
  it("shows saved configuration content immediately with no parent disclosure control", () => {
    // Given: a card with one power, one switch, and one gpio row
    view = mount(threeKindFixture());

    // When: the card renders without any interaction
    // Then: rows are reachable immediately with no collapsed parent region
    expect(configRow(view.host, "power.5v_out")).not.toBeNull();
    expect(configRow(view.host, "switch.tf")).not.toBeNull();
    expect(view.host.querySelector("[hidden]")).toBeNull();
    expect(view.host.querySelector("#persistent-config-content")).toBeNull();

    // Then: no parent disclosure button exists in any locale label form
    expect(parentDisclosureButton(view.host)).toBeUndefined();
    expect(view.host.querySelector('button[aria-controls="persistent-config-content"]')).toBeNull();
    expect(view.host.querySelector("button[aria-expanded]")).toBeNull();
  });

  it("opens power and switch groups by default and keeps the gpio group closed", () => {
    // Given: a card with one row per group kind
    view = mount(threeKindFixture());

    // When: the card renders without any interaction
    // Then: the nested details groups keep their established defaults
    expect(groupDetails(view.host, "power").open).toBe(true);
    expect(groupDetails(view.host, "switch").open).toBe(true);
    expect(groupDetails(view.host, "gpio").open).toBe(false);
  });

  it("keeps content visible across busy updates and presents busy feedback inline", () => {
    // Given: a mounted card with visible content
    view = mount(threeKindFixture());
    expect(view.host.querySelector("[hidden]")).toBeNull();

    // When: a busy state arrives
    const busy = new PersistentConfigApiError({ kind: "busy", activity: "capture" }, "busy");
    view.update(state({ config: config([powerItem("power.5v_out")]), error: busy }));

    // Then: the content stays visible and the busy feedback renders inline
    expect(view.host.querySelector("[hidden]")).toBeNull();
    expect(parentDisclosureButton(view.host)).toBeUndefined();
    expect(configRow(view.host, "power.5v_out")).not.toBeNull();
    expect(view.host.textContent).toContain("Power capture is using the board");
  });

  it("exposes no parent disclosure in Chinese either", () => {
    // Given: a card mounted in Chinese
    view = mount(threeKindFixture(), { lang: "zh" });

    // When: the card renders without any interaction
    // Then: no localized parent disclosure control or hidden region exists
    expect(parentDisclosureButton(view.host)).toBeUndefined();
    expect(view.host.querySelector("[hidden]")).toBeNull();
    expect(configRow(view.host, "power.5v_out")).not.toBeNull();
  });

  it("keeps row selection and mutation isolation when toggling a nested group", () => {
    // Given: a mounted card with spied mutations and a selected power row
    const fixture = threeKindFixture();
    view = mount(fixture);
    click(configRow(view.host, "power.5v_out"));
    expect(configRow(view.host, "power.5v_out").getAttribute("aria-checked")).toBe("true");

    // When: the closed gpio group is opened via its summary
    const gpioSummary = groupDetails(view.host, "gpio").querySelector("summary");
    if (!gpioSummary) throw new TypeError("GPIO group summary not found");
    click(gpioSummary);

    // Then: the group opens, the gpio row becomes reachable, and selection survives
    expect(groupDetails(view.host, "gpio").open).toBe(true);
    expect(configRow(view.host, "gpio.gp7")).not.toBeNull();
    expect(configRow(view.host, "power.5v_out").getAttribute("aria-checked")).toBe("true");

    // Then: no save, clear, or refresh call ever fired from group interaction
    expect(fixture.save).not.toHaveBeenCalled();
    expect(fixture.clear).not.toHaveBeenCalled();
    expect(fixture.refresh).not.toHaveBeenCalled();
  });
});
