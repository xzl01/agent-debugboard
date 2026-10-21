import { afterEach, describe, expect, it, vi } from "vitest";
import { parseGpioPhysicalLayout, validateGpioPhysicalJoin } from "./gpioPhysicalLayout";
import { directGpioFixtureCases, layoutEnvelope, physicalLayoutFixture } from "./gpioPhysicalLayout.testFixtures";
import { getGpioLayout } from "./api";

afterEach(() => vi.unstubAllGlobals());

describe("physical GPIO layout boundary", () => {
  it("retains the full matrix and sorts only groups, not coordinates", () => {
    const { layout, gpios } = physicalLayoutFixture();
    const parsed = parseGpioPhysicalLayout(layoutEnvelope(layout));
    expect(parsed.groups.map((group) => group.name)).toEqual(["J8", "J13"]);
    expect(parsed.groups.flatMap((group) => group.cells)).toHaveLength(24);
    expect(parsed.groups.flatMap((group) => group.cells).filter((cell) => cell.controllable)).toHaveLength(11);
    expect(parsed.groups.flatMap((group) => group.cells).filter((cell) => !cell.controllable)).toHaveLength(13);
    expect(() => validateGpioPhysicalJoin(parsed, gpios)).not.toThrow();
    expect(parsed.groups[0].cells[0]).toMatchObject({ pinNumber: 10, row: 0, column: 0 });
  });

  it.each(directGpioFixtureCases)("retains direct $gpioName capability/net metadata and joins by name", (entry) => {
    const { layout, gpios } = physicalLayoutFixture();
    const parsed = parseGpioPhysicalLayout(layoutEnvelope(layout));
    expect(parsed.groups[0].cells.find((cell) => cell.pinNumber === entry.pinNumber)).toMatchObject({
      label: entry.gpioName, signal: entry.signal, role: "gpio", gpioName: entry.gpioName, controllable: true, outputCapable: true,
    });
    expect(() => validateGpioPhysicalJoin(parsed, gpios)).not.toThrow();
    expect(() => validateGpioPhysicalJoin(parsed, gpios.filter((gpio) => gpio.name !== entry.gpioName))).toThrow(/unmatched controllable/);
    expect(() => validateGpioPhysicalJoin(parsed, [...gpios, { ...gpios[0], name: "GP17" }])).toThrow(/unmatched live/);
  });

  it.each([
    ["schema", "other"], ["ok", false], ["command", "status"], ["action", "list"],
  ])("rejects invalid envelope %s", (key, value) => {
    expect(() => parseGpioPhysicalLayout({ ...layoutEnvelope(physicalLayoutFixture().layout) as object, [key]: value })).toThrow();
  });

  it.each([
    ["wrong view", (r: any) => { r.layout.view = "bottom"; }],
    ["duplicate group", (r: any) => { r.layout.groups[1].name = "j13"; }],
    ["duplicate order", (r: any) => { r.layout.groups[1].order = 1; }],
    ["fractional order", (r: any) => { r.layout.groups[1].order = 0.5; }],
    ["zero rows", (r: any) => { r.layout.groups[0].rows = 0; }],
    ["huge dimensions", (r: any) => { r.layout.groups[0].columns = 65; }],
    ["missing cell", (r: any) => { r.layout.groups[0].cells.pop(); }],
    ["duplicate coordinate", (r: any) => { r.layout.groups[0].cells[1].column = 0; }],
    ["duplicate pin number", (r: any) => { r.layout.groups[0].cells[1].pinNumber = 2; }],
    ["out of range row", (r: any) => { r.layout.groups[0].cells[0].row = 2; }],
    ["negative column", (r: any) => { r.layout.groups[0].cells[0].column = -1; }],
    ["noninteger pin", (r: any) => { r.layout.groups[0].cells[0].pinNumber = 1.1; }],
    ["missing capability", (r: any) => { delete r.layout.groups[0].cells[0].outputCapable; }],
    ["output without control", (r: any) => { r.layout.groups[0].cells[3].outputCapable = true; }],
    ["missing GPIO name", (r: any) => { delete r.layout.groups[0].cells[0].gpioName; }],
    ["duplicate GPIO name", (r: any) => { r.layout.groups[0].cells[1].gpioName = "GP9"; }],
  ])("rejects %s", (_name, mutate) => {
    const response = layoutEnvelope(physicalLayoutFixture().layout);
    mutate(response);
    expect(() => parseGpioPhysicalLayout(response)).toThrow(/Invalid GPIO physical layout/);
  });

  it("joins by exact gpioName and rejects either unmatched side or duplicates", () => {
    const { layout, gpios } = physicalLayoutFixture();
    expect(() => validateGpioPhysicalJoin(layout, gpios.slice(1))).toThrow();
    expect(() => validateGpioPhysicalJoin(layout, [...gpios, { ...gpios[0], name: "future" }])).toThrow();
    expect(() => validateGpioPhysicalJoin(layout, [...gpios, gpios[0]])).toThrow();
    expect(() => validateGpioPhysicalJoin(layout, gpios.map((gpio) => ({ ...gpio, pin: 999 })))).not.toThrow();
  });

  it("retains an arbitrary future group and unknown inert role without a board map", () => {
    const response = layoutEnvelope({ view: "component-top", groups: [{ name: "FUTURE_A", edge: "diagonal", order: 73, rows: 1, columns: 1, cells: [
      { pinNumber: 101, row: 0, column: 0, label: "REF", signal: "TEST_REF", role: "future-reference", controllable: false, outputCapable: false },
    ] }] });
    const layout = parseGpioPhysicalLayout(response);
    expect(layout.groups[0].cells[0].role).toBe("future-reference");
    expect(() => validateGpioPhysicalJoin(layout, [])).not.toThrow();
  });

  it("GETs the frozen endpoint and forwards AbortSignal", async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify(layoutEnvelope(physicalLayoutFixture().layout))));
    vi.stubGlobal("fetch", fetchMock);
    const controller = new AbortController();
    expect((await getGpioLayout(controller.signal)).groups).toHaveLength(2);
    expect(fetchMock).toHaveBeenCalledWith("/api/v1/gpio/layout", expect.objectContaining({ signal: controller.signal }));
  });
});
