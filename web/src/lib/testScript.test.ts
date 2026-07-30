import { describe, it } from "node:test";
import assert from "node:assert/strict";
import {
  STEP_TYPES,
  stepTypeLabel,
  stepTypeIcon,
  generateStepId,
  generateLoopId,
  defaultStepParams,
  defaultScript,
  parseTestScript,
  serializeTestScript,
  isRunSuccessful,
  isTestLoop,
  isTestUnit,
  buildExecutionPlan,
  tryBuildExecutionPlan,
  nestItemInScript,
  removeNestedItemFromScript,
  compatibleAssertionForStepType,
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

  it("accepts skips caused only by an unselected condition branch", () => {
    assert.equal(isRunSuccessful({
      totalSteps: 2, passed: 1, failed: 0, skipped: 1, errored: 0,
      aborted: false, completed: true, durationMs: 1, startedAtMs: 0,
      finishedAtMs: 1,
      results: [
        result("pass"),
        { ...result("skip"), conditionalSkip: true },
      ],
    }), true);
  });

  it("rejects a run when safe cleanup fails", () => {
    assert.equal(isRunSuccessful({
      totalSteps: 1, passed: 1, failed: 0, skipped: 0, errored: 0,
      aborted: false, completed: false, durationMs: 1, startedAtMs: 0,
      finishedAtMs: 1, results: [result("pass")],
      cleanup: {
        attempted: true,
        passed: false,
        actions: [{ kind: "power", target: "5v_out", status: "error", error: "failed" }],
      },
    }), false);
  });
});

describe("compatibleAssertionForStepType", () => {
  it("retains generic and compatible assertion fields during type conversion", () => {
    assert.deepEqual(compatibleAssertionForStepType({
      continue_on_error: true,
      contains: "Linux",
      exit_code: 0,
      current_range: { min_a: 0, max_a: 1 },
    }, "serial_expect"), {
      continue_on_error: true,
      contains: "Linux",
      exit_code: 0,
    });
  });

  it("drops incompatible assertion fields instead of applying them to missing data", () => {
    assert.equal(compatibleAssertionForStepType({ contains: "Linux" }, "delay"), undefined);
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

  it("works on an HTTP origin where crypto.randomUUID is unavailable", () => {
    const cryptoDescriptor = Object.getOwnPropertyDescriptor(globalThis, "crypto");
    Object.defineProperty(globalThis, "crypto", {
      configurable: true,
      value: {
        getRandomValues: (bytes: Uint8Array) => {
          bytes.set([0x12, 0x34, 0x56, 0x78]);
          return bytes;
        },
      },
    });

    try {
      assert.equal(generateStepId(), "s12345678");
      assert.equal(generateLoopId(), "l12345678");
    } finally {
      if (cryptoDescriptor) Object.defineProperty(globalThis, "crypto", cryptoDescriptor);
      else Reflect.deleteProperty(globalThis, "crypto");
    }
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

describe("nestItemInScript", () => {
  const unit = {
    id: "stress-unit",
    type: "loop" as const,
    params: {
      count: 1,
      unit: { name: "Stress" },
      steps: [{ id: "stress", type: "delay" as const, params: { ms: 1000 } }],
    },
  };

  it("moves an existing top-level Unit into a loop body", () => {
    const loop = {
      id: "outer-loop",
      type: "loop" as const,
      params: {
        count: 2,
        steps: [{ id: "wait", type: "delay" as const, params: { ms: 10 } }],
      },
    };
    const result = nestItemInScript([unit, loop], unit, loop.id, "body", unit.id);
    assert.ok(result);
    assert.deepEqual(result.map((item) => item.id), [loop.id]);
    assert.equal(isTestLoop(result[0]) && result[0].params.steps[1].id, unit.id);
  });

  it("moves an existing top-level Unit into a condition branch", () => {
    const condition = {
      id: "gate",
      type: "condition" as const,
      params: {
        check: { id: "check", type: "gpio_assert" as const, params: { pin: "GP13", direction: "input" as const, value: 1 as const } },
        then_steps: [],
        else_steps: [],
      },
    };
    const result = nestItemInScript([condition, unit], unit, condition.id, "else", unit.id);
    assert.ok(result);
    assert.equal(result.length, 1);
    assert.equal(result[0].type === "condition" && result[0].params.else_steps[0].id, unit.id);
  });

  it("copies a primitive palette step into a loop body", () => {
    const loop = {
      id: "outer-loop",
      type: "loop" as const,
      params: { count: 2, steps: [] },
    };
    const step = { id: "new-delay", type: "delay" as const, params: { ms: 250 } };
    const result = nestItemInScript([loop], step, loop.id, "body");
    assert.ok(result);
    assert.equal(isTestLoop(result[0]) && result[0].params.steps[0].id, step.id);
  });

  it("moves an existing primitive step into a condition branch", () => {
    const step = { id: "power-on", type: "power_on" as const, params: { rail: "5v_out" } };
    const condition = {
      id: "gate",
      type: "condition" as const,
      params: {
        check: { id: "check", type: "gpio_assert" as const, params: { pin: "GP13", direction: "input" as const, value: 1 as const } },
        then_steps: [],
        else_steps: [],
      },
    };
    const result = nestItemInScript([step, condition], step, condition.id, "then", step.id);
    assert.ok(result);
    assert.equal(result.length, 1);
    assert.equal(result[0].type === "condition" && result[0].params.then_steps[0].id, step.id);
  });

  it("rejects invalid targets without removing the source item", () => {
    const step = { id: "delay", type: "delay" as const, params: { ms: 1 } };
    assert.equal(nestItemInScript([unit, step], unit, step.id, "body", unit.id), null);
  });
});

describe("removeNestedItemFromScript", () => {
  it("removes a child from a loop body without mutating the source", () => {
    const loop = {
      id: "outer-loop",
      type: "loop" as const,
      params: {
        count: 2,
        steps: [
          { id: "first", type: "delay" as const, params: { ms: 10 } },
          { id: "second", type: "delay" as const, params: { ms: 20 } },
        ],
      },
    };
    const result = removeNestedItemFromScript([loop], loop.id, "body", "first");
    assert.ok(result);
    assert.equal(isTestLoop(result[0]) && result[0].params.steps.length, 1);
    assert.equal(isTestLoop(result[0]) && result[0].params.steps[0].id, "second");
    assert.equal(loop.params.steps.length, 2);
  });

  it("removes only the requested condition branch item", () => {
    const condition = {
      id: "gate",
      type: "condition" as const,
      params: {
        check: { id: "check", type: "gpio_assert" as const, params: { pin: "GP13", direction: "input" as const, value: 1 as const } },
        then_steps: [{ id: "then-delay", type: "delay" as const, params: { ms: 10 } }],
        else_steps: [{ id: "else-delay", type: "delay" as const, params: { ms: 20 } }],
      },
    };
    const result = removeNestedItemFromScript([condition], condition.id, "else", "else-delay");
    assert.ok(result);
    assert.equal(result[0].type === "condition" && result[0].params.then_steps.length, 1);
    assert.equal(result[0].type === "condition" && result[0].params.else_steps.length, 0);
  });

  it("returns null when the nested item is not present", () => {
    const loop = {
      id: "outer-loop",
      type: "loop" as const,
      params: { count: 2, steps: [] },
    };
    assert.equal(removeNestedItemFromScript([loop], loop.id, "body", "missing"), null);
  });

  it("allows the editor to remove the last child without throwing during render", () => {
    const loop = {
      id: "outer-loop",
      type: "loop" as const,
      params: {
        count: 2,
        steps: [{ id: "only-child", type: "delay" as const, params: { ms: 10 } }],
      },
    };
    const result = removeNestedItemFromScript([loop], loop.id, "body", "only-child");
    assert.ok(result);
    assert.equal(isTestLoop(result[0]) && result[0].params.steps.length, 0);

    const attempt = tryBuildExecutionPlan({
      schema: "linkr-test.v1",
      name: "Incomplete editor draft",
      version: "1.0",
      steps: result,
    });
    assert.deepEqual(attempt.plan, []);
    assert.match(attempt.error ?? "", /at least one step is required/);
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
    assert.equal(isTestLoop(parsed.steps[1]), false);
    assert.equal((parsed.steps[1] as import("./testScript.ts").TestStep<"delay">).params.ms, 2000);
    assert.deepEqual(
      (parsed.steps[2] as import("./testScript.ts").TestStep).assert?.current_range,
      { min_a: 0, max_a: 3 },
    );
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
    assert.equal((parsed.steps[0] as import("./testScript.ts").TestStep).assert, undefined);
    assert.equal((parsed.steps[0] as import("./testScript.ts").TestStep).continue_on_error, undefined);
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

  it("rejects step with non-object params", () => {
    const ndjson = [
      JSON.stringify({ schema: "linkr-test.v1", name: "Bad" }),
      JSON.stringify({ id: "s1", type: "delay", params: "invalid" }),
    ].join("\n") + "\n";
    assert.throws(() => parseTestScript(ndjson), /params.*must be an object/);
  });

  it("preserves continue_on_error", () => {
    const ndjson = [
      JSON.stringify({ schema: "linkr-test.v1", name: "COE" }),
      JSON.stringify({ id: "s1", type: "delay", params: { ms: 100 }, continue_on_error: true }),
    ].join("\n") + "\n";
    const parsed = parseTestScript(ndjson);
    assert.equal((parsed.steps[0] as import("./testScript.ts").TestStep).continue_on_error, true);
  });

  it("round-trips loop blocks and expands each iteration with unique execution IDs", () => {
    const original = defaultScript();
    original.steps = [
      {
        id: "loop1",
        type: "loop",
        params: {
          count: 3,
          steps: [
            { id: "send", type: "serial_send", params: { channel: "uart0", text: "ping\n" } },
            { id: "delay", type: "delay", params: { ms: 10 } },
          ],
        },
      },
    ];

    const parsed = parseTestScript(serializeTestScript(original));
    assert.equal(isTestLoop(parsed.steps[0]), true);
    if (!isTestLoop(parsed.steps[0])) throw new Error("expected loop");
    assert.equal(parsed.steps[0].params.count, 3);
    assert.equal(parsed.steps[0].params.steps.length, 2);

    const plan = buildExecutionPlan(parsed);
    assert.equal(plan.length, 6);
    assert.deepEqual(plan.map((step) => step.executionId), [
      "send@loop1:1",
      "delay@loop1:1",
      "send@loop1:2",
      "delay@loop1:2",
      "send@loop1:3",
      "delay@loop1:3",
    ]);
    assert.deepEqual(plan.map((step) => step.loopIteration), [1, 1, 2, 2, 3, 3]);
  });

  it("round-trips a named Unit and executes its packaged steps once", () => {
    const original = defaultScript();
    original.steps = [
      {
        id: "unit1",
        type: "loop",
        params: {
          count: 1,
          unit: { name: "Stress test" },
          steps: [
            { id: "login", type: "serial_wait", params: { channel: "uart0", pattern: "# ", timeout_ms: 30000 } },
            { id: "stress", type: "serial_expect", params: { channel: "uart0", command: "stress-ng --cpu 4 --timeout 30s", pattern: "", timeout_ms: 40000 } },
          ],
        },
      },
    ];

    const parsed = parseTestScript(serializeTestScript(original));
    assert.equal(isTestUnit(parsed.steps[0]), true);
    if (!isTestUnit(parsed.steps[0])) throw new Error("expected Unit");
    assert.equal(parsed.steps[0].params.unit.name, "Stress test");
    const plan = buildExecutionPlan(parsed);
    assert.equal(plan.length, 2);
    assert.deepEqual(plan.map((step) => step.unitId), ["unit1", "unit1"]);
    assert.deepEqual(plan.map((step) => step.unitName), ["Stress test", "Stress test"]);
  });

  it("round-trips condition branches with stable execution metadata", () => {
    const original = defaultScript();
    original.steps = [{
      id: "condition1",
      type: "condition",
      params: {
        check: {
          id: "check",
          type: "adc_read",
          params: { channel: "5v_out" },
          assert: { current_range: { min_a: 0.1, max_a: 1 } },
        },
        then_steps: [{ id: "then", type: "delay", params: { ms: 10 } }],
        else_steps: [{ id: "else", type: "power_off", params: { rail: "5v_out" } }],
      },
    }];

    const parsed = parseTestScript(serializeTestScript(original));
    assert.equal(parsed.steps[0].type, "condition");
    const plan = buildExecutionPlan(parsed);
    assert.deepEqual(plan.map((step) => step.executionId), [
      "check@condition1:check",
      "then@condition1:then",
      "else@condition1:else",
    ]);
    assert.deepEqual(plan.map((step) => step.conditionRole), ["check", "then", "else"]);
  });

  it("allows named Units inside a loop without losing Unit or iteration identity", () => {
    const header = JSON.stringify({ schema: "linkr-test.v1", name: "Looped Unit" });
    const loop = JSON.stringify({
      id: "outer",
      type: "loop",
      params: {
        count: 2,
        steps: [{
          id: "stress-unit",
          type: "loop",
          params: {
            count: 1,
            unit: { name: "Stress" },
            steps: [
              { id: "run", type: "serial_expect", params: { channel: "uart0", command: "stress-ng", pattern: "", timeout_ms: 1000 } },
              { id: "cooldown", type: "delay", params: { ms: 10 } },
            ],
          },
        }],
      },
    });
    const parsed = parseTestScript(`${header}\n${loop}\n`);
    const plan = buildExecutionPlan(parsed);
    assert.deepEqual(plan.map((step) => step.executionId), [
      "run@stress-unit:1@outer:1",
      "cooldown@stress-unit:1@outer:1",
      "run@stress-unit:1@outer:2",
      "cooldown@stress-unit:1@outer:2",
    ]);
    assert.deepEqual(plan.map((step) => step.unitName), ["Stress", "Stress", "Stress", "Stress"]);
    assert.deepEqual(plan.map((step) => step.loopIteration), [1, 1, 2, 2]);
    assert.equal(serializeTestScript(parsed).includes('"unit":{"name":"Stress"}'), true);
  });

  it("allows named Units in both condition branches and preserves branch identity", () => {
    const header = JSON.stringify({ schema: "linkr-test.v1", name: "Conditional Unit" });
    const unit = (id: string, name: string) => ({
      id,
      type: "loop",
      params: {
        count: 1,
        unit: { name },
        steps: [{ id: `${id}-delay`, type: "delay", params: { ms: 1 } }],
      },
    });
    const condition = JSON.stringify({
      id: "gate",
      type: "condition",
      params: {
        check: { id: "check", type: "gpio_assert", params: { pin: "GP13", direction: "input", value: 1 } },
        then_steps: [unit("pass-unit", "Pass path")],
        else_steps: [unit("fail-unit", "Fail path")],
      },
    });
    const plan = buildExecutionPlan(parseTestScript(`${header}\n${condition}\n`));
    assert.deepEqual(plan.map((step) => step.executionId), [
      "check@gate:check",
      "pass-unit-delay@pass-unit:1@gate:then",
      "fail-unit-delay@fail-unit:1@gate:else",
    ]);
    assert.deepEqual(plan.map((step) => step.conditionRole), ["check", "then", "else"]);
    assert.deepEqual(plan.map((step) => step.unitName), [undefined, "Pass path", "Fail path"]);
  });

  it("rejects unsupported condition checks and duplicate child IDs", () => {
    const header = JSON.stringify({ schema: "linkr-test.v1", name: "Bad condition" });
    assert.throws(() => parseTestScript([
      header,
      JSON.stringify({
        id: "condition1",
        type: "condition",
        params: {
          check: { id: "check", type: "delay", params: { ms: 1 } },
          then_steps: [],
          else_steps: [],
        },
      }),
    ].join("\n")), /unsupported (condition )?check type/);
    assert.throws(() => parseTestScript([
      header,
      JSON.stringify({
        id: "condition1",
        type: "condition",
        params: {
          check: { id: "same", type: "gpio_assert", params: { pin: "GP13", direction: "input", value: 0 } },
          then_steps: [{ id: "same", type: "delay", params: { ms: 1 } }],
          else_steps: [],
        },
      }),
    ].join("\n")), /duplicate child step ID/);
    assert.throws(() => parseTestScript([
      header,
      JSON.stringify({
        id: "condition1",
        type: "condition",
        params: {
          check: { id: "check", type: "adc_read", params: { channel: "5v_out" } },
          then_steps: [],
          else_steps: [],
        },
      }),
    ].join("\n")), /adc_read check requires an assert/);
  });

  it("loads incomplete editor drafts without requiring a valid execution plan", () => {
    const header = JSON.stringify({ schema: "linkr-test.v1", name: "Draft" });
    const loop = JSON.stringify({
      id: "loop1",
      type: "loop",
      params: { count: 2, steps: [] },
    });
    const draft = parseTestScript(`${header}\n${loop}\n`, { validatePlan: false });
    assert.equal(draft.steps[0].type, "loop");
    assert.equal(isTestLoop(draft.steps[0]) && draft.steps[0].params.steps.length, 0);
    assert.throws(() => parseTestScript(`${header}\n${loop}\n`), /non-empty array|at least one step is required/);
    assert.match(
      tryBuildExecutionPlan(draft).error ?? "",
      /at least one step is required/,
    );

    const bareAdc = JSON.stringify({
      id: "condition1",
      type: "condition",
      params: {
        check: { id: "check", type: "adc_read", params: { channel: "5v_out" } },
        then_steps: [],
        else_steps: [],
      },
    });
    const incompleteCondition = parseTestScript(`${header}\n${bareAdc}\n`, { validatePlan: false });
    assert.equal(incompleteCondition.steps[0].type, "condition");
    assert.throws(() => parseTestScript(`${header}\n${bareAdc}\n`), /adc_read check requires an assert/);
    assert.match(
      tryBuildExecutionPlan(incompleteCondition).error ?? "",
      /adc_read check requires an assert/,
    );
  });

  it("rejects empty serial_wait patterns and empty condition serial_expect patterns", () => {
    const header = JSON.stringify({ schema: "linkr-test.v1", name: "Empty pattern" });
    assert.throws(() => parseTestScript([
      header,
      JSON.stringify({
        id: "wait1",
        type: "serial_wait",
        params: { channel: "uart0", pattern: "   ", timeout_ms: 1000 },
      }),
    ].join("\n")), /serial_wait pattern must be non-empty/);
    assert.throws(() => parseTestScript([
      header,
      JSON.stringify({
        id: "condition1",
        type: "condition",
        params: {
          check: {
            id: "check",
            type: "serial_expect",
            params: { channel: "uart0", command: "true", pattern: "", timeout_ms: 100 },
          },
          then_steps: [],
          else_steps: [],
        },
      }),
    ].join("\n")), /serial_expect check pattern must be non-empty/);
  });

  it("rejects invalid Unit names before execution", () => {
    const header = JSON.stringify({ schema: "linkr-test.v1", name: "Bad Unit" });
    const unit = JSON.stringify({
      id: "unit1",
      type: "loop",
      params: {
        count: 1,
        unit: { name: "" },
        steps: [{ id: "delay", type: "delay", params: { ms: 1 } }],
      },
    });
    assert.throws(() => parseTestScript(`${header}\n${unit}\n`), /unit "name"/);
  });

  it("rejects Unit repeat counts so repeated side effects cannot be hidden", () => {
    const header = JSON.stringify({ schema: "linkr-test.v1", name: "Repeated Unit" });
    const unit = JSON.stringify({
      id: "unit1",
      type: "loop",
      params: {
        count: 2,
        unit: { name: "Power cycle" },
        steps: [{ id: "off", type: "power_off", params: { rail: "5v_out" } }],
      },
    });
    assert.throws(() => parseTestScript(`${header}\n${unit}\n`), /unit "count" must be exactly 1/);
  });

  it("rejects invalid, empty, and nested loop blocks", () => {
    const header = JSON.stringify({ schema: "linkr-test.v1", name: "Bad loop" });
    assert.throws(() => parseTestScript([
      header,
      JSON.stringify({ id: "loop1", type: "loop", params: { count: 0, steps: [{}] } }),
    ].join("\n")), /loop "count"/);
    assert.throws(() => parseTestScript([
      header,
      JSON.stringify({ id: "loop1", type: "loop", params: { count: 2, steps: [] } }),
    ].join("\n")), /non-empty array/);
    assert.throws(() => parseTestScript([
      header,
      JSON.stringify({
        id: "loop1",
        type: "loop",
        params: { count: 2, steps: [{ id: "loop2", type: "loop", params: { count: 2, steps: [] } }] },
      }),
    ].join("\n")), /only named Units may be nested/);
    assert.throws(() => parseTestScript([
      header,
      JSON.stringify({
        id: "loop1",
        type: "loop",
        params: {
          count: 1000,
          steps: Array.from({ length: 11 }, (_, index) => ({
            id: `s${index}`,
            type: "delay",
            params: { ms: 1 },
          })),
        },
      }),
    ].join("\n")), /10000 executable steps/);
  });

  it("rejects duplicate script, loop child, and execution IDs", () => {
    const header = JSON.stringify({ schema: "linkr-test.v1", name: "Duplicate IDs" });
    assert.throws(() => parseTestScript([
      header,
      JSON.stringify({ id: "same", type: "delay", params: { ms: 1 } }),
      JSON.stringify({ id: "same", type: "delay", params: { ms: 1 } }),
    ].join("\n")), /Duplicate script item ID: same/);
    assert.throws(() => parseTestScript([
      header,
      JSON.stringify({
        id: "loop1",
        type: "loop",
        params: {
          count: 2,
          steps: [
            { id: "same", type: "delay", params: { ms: 1 } },
            { id: "same", type: "delay", params: { ms: 1 } },
          ],
        },
      }),
    ].join("\n")), /duplicate child step ID: same/);
    assert.throws(() => parseTestScript([
      header,
      JSON.stringify({ id: "child@loop1:1", type: "delay", params: { ms: 1 } }),
      JSON.stringify({
        id: "loop1",
        type: "loop",
        params: { count: 1, steps: [{ id: "child", type: "delay", params: { ms: 1 } }] },
      }),
    ].join("\n")), /Duplicate execution step ID: child@loop1:1/);
  });

  it("auto-generates created field on serialize", () => {
    const script = defaultScript();
    const ndjson = serializeTestScript(script);
    const header = JSON.parse(ndjson.split("\n")[0]);
    assert.ok(header.created, "created field should be present");
    assert.match(header.created, /^\d{4}-\d{2}-\d{2}T/);
  });
});
