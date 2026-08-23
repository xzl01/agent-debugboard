import { afterEach, beforeEach, describe, expect, it } from "vitest";
import {
  catalogRowIds,
  mountTaskCard,
  setupTaskCardHarness,
  teardownTaskCardHarness,
} from "./TaskCard.testUtils";

beforeEach(setupTaskCardHarness);
afterEach(teardownTaskCardHarness);

describe("TaskCard built-in catalog layout contract", () => {
  it("renders the six built-in tasks as ordinary rows in the frozen order", async () => {
    // Given: the card rendered against a connected board serving the firmware catalog
    const view = await mountTaskCard();

    // Then: exactly the six built-ins, in catalog order
    expect(catalogRowIds(view)).toEqual([
      "builtin/maskrom/5v_out",
      "builtin/maskrom/12v_out",
      "builtin/maskrom/20v_out",
      "builtin/edl/5v_out",
      "builtin/edl/12v_out",
      "builtin/edl/20v_out",
    ]);
  });

  it("marks every built-in row with the localized built-in badge", async () => {
    // Given: the rendered card
    const view = await mountTaskCard();

    // Then: each of the six rows carries the built-in badge and one generic run action
    const rows = [...view.querySelectorAll<HTMLElement>("div.rounded-lg")];
    expect(rows).toHaveLength(6);
    for (const row of rows) {
      expect(row.textContent).toContain("Built-in");
      const buttons = [...row.querySelectorAll("button")];
      expect(buttons.map((button) => button.textContent?.trim())).toEqual(["Run task"]);
    }
  });

  it("offers no recovery mode or rail selectors and no preset load/store actions", async () => {
    // Given: the rendered card
    const view = await mountTaskCard();

    // Then: the dedicated recovery UI is gone entirely
    expect(view.querySelectorAll("select")).toHaveLength(0);
    expect(view.textContent).not.toContain("Load task preset");
    expect(view.textContent).not.toContain("Store task preset");
    expect(view.textContent).not.toContain("Target platform");
    expect(view.textContent).not.toContain("Target power rail");
  });

  it("keeps the custom current-workflow storage section", async () => {
    // Given: the rendered card
    const view = await mountTaskCard();

    // Then: the task id input and store action remain available
    expect(view.querySelector("input")).not.toBeNull();
    expect(view.textContent).toContain("Store current workflow");
  });
});
