import { describe, it } from "node:test";
import assert from "node:assert/strict";
import {
  STEP_TYPES,
  stepTypeLabel,
  stepTypeIcon,
  generateStepId,
  defaultStepParams,
  defaultScript,
  parseTestScript,
  serializeTestScript,
  isRunSuccessful,
} from "./testScript.ts";

describe("stepTypeLabel", () => {
  it("returns a label for every step type", () => {
    for (const type of STEP_TYPES) {
      const label = stepTypeLabel(type);
      assert.ok(label.length > 0, `empty label for ${type}`);
      assert.notEqual(label, type, `label should differ from type for ${type}`);
    }
  });
});

describe("isRunSuccessful", () => {
  const result = (status: "pass" | "skip" | "aborted") => ({
    stepId: status,
    stepType: "delay" as const,
    status,
    startedAtMs: 0,
    finishedAtMs: 1,
    durationMs: 1,
  });

  it("accepts only a complete all-pass run", () => {
    assert.equal(isRunSuccessful({
      totalSteps: 1, passed: 1, failed: 0, skipped: 0, errored: 0,
      aborted: false, completed: true, durationMs: 1, startedAtMs: 0,
      finishedAtMs: 1, results: [result("pass")],
    }), true);
  });

  it("rejects skipped and aborted partial runs", () => {
    assert.equal(isRunSuccessful({
      totalSteps: 2, passed: 1, failed: 0, skipped: 1, errored: 0,
      aborted: false, completed: true, durationMs: 1, startedAtMs: 0,
      finishedAtMs: 1, results: [result("pass"), result("skip")],
    }), false);
    assert.equal(isRunSuccessful({
      totalSteps: 2, passed: 1, failed: 0, skipped: 0, errored: 0,
      aborted: true, completed: false, durationMs: 1, startedAtMs: 0,
      finishedAtMs: 1, results: [result("pass"), result("aborted")],
    }), false);
  });
});

describe("stepTypeIcon", () => {
  it("returns an icon name for every step type", () => {
    for (const type of STEP_TYPES) {
      const icon = stepTypeIcon(type);
      assert.ok(icon.length > 0, `empty icon for ${type}`);
    }
  });

  it("does not return 'Waveform' (invalid lucide icon)", () => {
    for (const type of STEP_TYPES) {
      assert.notEqual(stepTypeIcon(type), "Waveform");
    }
  });
});

describe("generateStepId", () => {
  it("generates unique IDs", () => {
    const ids = new Set<string>();
    for (let i = 0; i < 100; i++) {
      ids.add(generateStepId());
    }
    assert.equal(ids.size, 100);
  });

  it("starts with 's' prefix", () => {
    const id = generateStepId();
    assert.ok(id.startsWith("s"));
  });
});

describe("defaultStepParams", () => {
  it("returns params for every step type", () => {
    for (const type of STEP_TYPES) {
      const params = defaultStepParams(type);
      assert.ok(params !== null && typeof params === "object");
      assert.ok(Object.keys(params).length > 0, `empty params for ${type}`);
    }
  });

  it("returns rail for power steps", () => {
    assert.equal(defaultStepParams("power_on").rail, "5v_out");
    assert.equal(defaultStepParams("power_off").rail, "5v_out");
  });

  it("returns ms for delay", () => {
    assert.equal(defaultStepParams("delay").ms, 1000);
  });
});

describe("defaultScript", () => {
  it("creates a valid script with correct schema", () => {
    const script = defaultScript();
    assert.equal(script.schema, "linkr-test.v1");
    assert.equal(script.name, "New Test");
    assert.deepEqual(script.steps, []);
  });
});

describe("serializeTestScript / parseTestScript", () => {
  it("round-trips a script with steps", () => {
    const original = defaultScript();
    original.name = "Test Round Trip";
    original.steps = [
      { id: "s1", type: "power_on", params: { rail: "5v_out" } },
      { id: "s2", type: "delay", params: { ms: 2000 } },
      { id: "s3", type: "adc_read", params: { channel: "12v_out" }, assert: { current_range: { min_a: 0, max_a: 3 } } },
    ];

    const ndjson = serializeTestScript(original);
    const parsed = parseTestScript(ndjson);

    assert.equal(parsed.schema, "linkr-test.v1");
    assert.equal(parsed.name, "Test Round Trip");
    assert.equal(parsed.steps.length, 3);
    assert.equal(parsed.steps[0].type, "power_on");
    assert.equal(parsed.steps[1].params.ms, 2000);
    assert.deepEqual(parsed.steps[2].assert?.current_range, { min_a: 0, max_a: 3 });
  });

  it("rejects empty input", () => {
    assert.throws(() => parseTestScript(""), /Empty script/);
  });

  it("rejects unknown schema", () => {
    const ndjson = JSON.stringify({ schema: "unknown.v1", name: "Bad" }) + "\n";
    assert.throws(() => parseTestScript(ndjson), /Unknown schema/);
  });

  it("handles missing optional fields", () => {
    const ndjson = [
      JSON.stringify({ schema: "linkr-test.v1", name: "Minimal" }),
      JSON.stringify({ id: "s1", type: "delay", params: { ms: 100 } }),
    ].join("\n") + "\n";

    const parsed = parseTestScript(ndjson);
    assert.equal(parsed.steps[0].assert, undefined);
    assert.equal(parsed.steps[0].continue_on_error, undefined);
  });

  it("rejects step with invalid type", () => {
    const ndjson = [
      JSON.stringify({ schema: "linkr-test.v1", name: "Bad" }),
      JSON.stringify({ id: "s1", type: "bogus", params: {} }),
    ].join("\n") + "\n";
    assert.throws(() => parseTestScript(ndjson), /unknown step type/);
  });

  it("rejects step with missing id", () => {
    const ndjson = [
      JSON.stringify({ schema: "linkr-test.v1", name: "Bad" }),
      JSON.stringify({ type: "delay", params: { ms: 100 } }),
    ].join("\n") + "\n";
    assert.throws(() => parseTestScript(ndjson), /missing or invalid "id"/);
  });

  it("preserves continue_on_error", () => {
    const ndjson = [
      JSON.stringify({ schema: "linkr-test.v1", name: "COE" }),
      JSON.stringify({ id: "s1", type: "delay", params: { ms: 100 }, continue_on_error: true }),
    ].join("\n") + "\n";
    const parsed = parseTestScript(ndjson);
    assert.equal(parsed.steps[0].continue_on_error, true);
  });

  it("auto-generates created field on serialize", () => {
    const script = defaultScript();
    const ndjson = serializeTestScript(script);
    const header = JSON.parse(ndjson.split("\n")[0]);
    assert.ok(header.created, "created field should be present");
    assert.match(header.created, /^\d{4}-\d{2}-\d{2}T/);
  });
});
