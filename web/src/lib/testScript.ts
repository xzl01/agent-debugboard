export type StepType =
  | "power_on"
  | "power_off"
  | "delay"
  | "serial_wait"
  | "serial_send"
  | "serial_expect"
  | "adc_read"
  | "gpio_set"
  | "gpio_assert"
  | "switch_route"
  | "capture";

export type StepStatus = "pending" | "running" | "pass" | "fail" | "skip" | "error" | "aborted";

export type SerialChannel = "uart0" | "uart1";
export type SwitchName = "sd" | "usb" | "vin";
export type CaptureTrigger = "manual" | "current" | "gpio" | "power_on";

export interface PowerOnParams { rail: string }
export interface PowerOffParams { rail: string }
export interface DelayParams { ms: number }
export interface SerialWaitParams { channel: SerialChannel; pattern: string; timeout_ms: number }
export interface SerialSendParams { channel: SerialChannel; text: string }
export interface SerialExpectParams { channel: SerialChannel; command: string; pattern: string; timeout_ms: number }
export interface AdcReadParams { channel: string }
export interface GpioSetParams { pin: string; value: 0 | 1 }
export interface GpioAssertParams { pin: string; direction: "input" | "output"; value: 0 | 1 }
export interface SwitchRouteParams { switch: SwitchName; route: string }
export interface CaptureParams {
  rail: string;
  trigger: CaptureTrigger;
  duration_ms: number;
  threshold_a?: number;
}

export type StepParamsMap = {
  power_on: PowerOnParams;
  power_off: PowerOffParams;
  delay: DelayParams;
  serial_wait: SerialWaitParams;
  serial_send: SerialSendParams;
  serial_expect: SerialExpectParams;
  adc_read: AdcReadParams;
  gpio_set: GpioSetParams;
  gpio_assert: GpioAssertParams;
  switch_route: SwitchRouteParams;
  capture: CaptureParams;
};

export type StepParamsFor<T extends StepType> = StepParamsMap[T];

// Narrowed step type for use in switch cases
export type NarrowedTestStep<T extends StepType> = { id: string; type: T; params: StepParamsFor<T>; assert?: StepAssertion; continue_on_error?: boolean };

export interface TestStep<T extends StepType = StepType> {
  id: string;
  type: T;
  params: StepParamsFor<T>;
  assert?: StepAssertion;
  continue_on_error?: boolean;
}

export const MIN_LOOP_COUNT = 1;
export const MAX_LOOP_COUNT = 1000;
export const MAX_EXECUTION_STEPS = 10_000;

export interface TestLoop {
  id: string;
  type: "loop";
  params: {
    count: number;
    steps: TestNestedItem[];
    unit?: {
      name: string;
    };
  };
}

export type ConditionCheckType = "serial_expect" | "adc_read" | "gpio_assert";

export interface TestCondition {
  id: string;
  type: "condition";
  params: {
    check: TestStep<ConditionCheckType>;
    then_steps: TestNestedItem[];
    else_steps: TestNestedItem[];
  };
}

/** Nested flows accept primitive steps and named Units, but not conditions or bare loops. */
export type TestNestedItem = TestStep | TestLoop;
export type TestScriptItem = TestStep | TestLoop | TestCondition;
export type TestNestedBranch = "body" | "then" | "else";

export interface ExecutionStep extends TestStep {
  executionId: string;
  sourceStepId: string;
  loopId?: string;
  loopIteration?: number;
  loopCount?: number;
  unitId?: string;
  unitName?: string;
  conditionId?: string;
  conditionRole?: "check" | "then" | "else";
}

export interface ExecutionPlanAttempt {
  plan: ExecutionStep[];
  error: string | null;
}

export interface TestScript {
  schema: "linkr-test.v1";
  name: string;
  version: string;
  board?: string;
  created?: string;
  steps: TestScriptItem[];
}

export interface StepAssertion {
  continue_on_error?: boolean;
  current_range?: { min_a: number; max_a: number };
  contains?: string;
  regex?: string;
  exit_code?: number;
  pin_direction?: "input" | "output";
  pin_value?: 0 | 1;
  peak_current_max_a?: number;
  energy_max_j?: number;
}

export interface StepResult {
  stepId: string;
  sourceStepId?: string;
  loopId?: string;
  loopIteration?: number;
  loopCount?: number;
  unitId?: string;
  unitName?: string;
  conditionId?: string;
  conditionRole?: "check" | "then" | "else";
  conditionOutcome?: boolean;
  conditionalSkip?: boolean;
  stepType: StepType;
  status: StepStatus;
  startedAtMs: number;
  finishedAtMs: number;
  durationMs: number;
  error?: string;
  assertionResult?: { passed: boolean; detail: string };
  adcValueUa?: number;
  serialOutput?: string;
  captureId?: number;
}

export interface RunSummary {
  totalSteps: number;
  passed: number;
  failed: number;
  skipped: number;
  errored: number;
  aborted: boolean;
  completed: boolean;
  durationMs: number;
  startedAtMs: number;
  finishedAtMs: number;
  results: StepResult[];
  cleanup?: CleanupSummary;
  infrastructureError?: string;
}

export interface CleanupActionResult {
  kind: "capture" | "gpio" | "power";
  target: string;
  status: "pass" | "error";
  error?: string;
}

export interface CleanupSummary {
  attempted: boolean;
  passed: boolean;
  actions: CleanupActionResult[];
}

export function isRunSuccessful(summary: RunSummary): boolean {
  return summary.completed && !summary.aborted && !summary.infrastructureError && summary.cleanup?.passed !== false &&
    summary.results.length === summary.totalSteps &&
    summary.results.every((result) => result.status === "pass" || (
      result.status === "skip" && result.conditionalSkip === true
    ));
}

export interface AdcSampleEntry {
  stepId: string;
  channel: string;
  currentUa: number;
  timestampMs: number;
}

export interface PowerCaptureEvidenceEntry {
  stepId: string;
  rail: string;
  capture: PowerCapture;
}

export interface SerialLogEntry {
  stepId: string;
  channel: SerialChannel;
  text: string;
  direction: "rx" | "tx";
  timestampMs: number;
}

const ASSERTION_FIELDS_BY_STEP: Partial<Record<StepType, ReadonlySet<keyof StepAssertion>>> = {
  adc_read: new Set(["current_range"]),
  serial_wait: new Set(["contains", "regex"]),
  serial_expect: new Set(["contains", "regex", "exit_code"]),
  gpio_assert: new Set(["pin_direction", "pin_value"]),
  capture: new Set(["peak_current_max_a", "energy_max_j"]),
};

/** Preserve only assertion fields that remain meaningful after a step type change. */
export function compatibleAssertionForStepType(
  assertion: StepAssertion | undefined,
  type: StepType,
): StepAssertion | undefined {
  if (!assertion) return undefined;
  const allowed = ASSERTION_FIELDS_BY_STEP[type] ?? new Set<keyof StepAssertion>();
  const next: StepAssertion = {};
  if (assertion.continue_on_error != null) next.continue_on_error = assertion.continue_on_error;
  for (const key of allowed) {
    const value = assertion[key];
    if (value != null) Object.assign(next, { [key]: value });
  }
  return Object.keys(next).length > 0 ? next : undefined;
}

export const STEP_TYPES: StepType[] = [
  "power_on",
  "power_off",
  "delay",
  "serial_wait",
  "serial_send",
  "serial_expect",
  "adc_read",
  "gpio_set",
  "gpio_assert",
  "switch_route",
  "capture",
];

const LABELS: Record<StepType, string> = {
  power_on: "Power On",
  power_off: "Power Off",
  delay: "Delay",
  serial_wait: "Serial Wait",
  serial_send: "Serial Send",
  serial_expect: "Serial Expect",
  adc_read: "ADC Read",
  gpio_set: "GPIO Set",
  gpio_assert: "GPIO Assert",
  switch_route: "Switch Route",
  capture: "Power Capture",
};

const ICONS: Record<StepType, string> = {
  power_on: "Zap",
  power_off: "Power",
  delay: "Timer",
  serial_wait: "Hourglass",
  serial_send: "Send",
  serial_expect: "MessageSquareText",
  adc_read: "Activity",
  gpio_set: "ToggleRight",
  gpio_assert: "CheckCircle",
  switch_route: "GitBranch",
  capture: "Activity",
};

export function stepTypeLabel(type: StepType): string {
  return LABELS[type] ?? type;
}

export function stepTypeIcon(type: StepType): string {
  return ICONS[type] ?? "Circle";
}

function generateItemId(prefix: "s" | "l" | "c"): string {
  if (typeof globalThis.crypto?.randomUUID === "function") {
    return `${prefix}${globalThis.crypto.randomUUID().slice(0, 8)}`;
  }

  const bytes = new Uint8Array(4);
  if (typeof globalThis.crypto?.getRandomValues === "function") {
    globalThis.crypto.getRandomValues(bytes);
  } else {
    for (let i = 0; i < bytes.length; i += 1) {
      bytes[i] = Math.floor(Math.random() * 256);
    }
  }
  return `${prefix}${Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("")}`;
}

export function generateStepId(): string {
  return generateItemId("s");
}

export function generateLoopId(): string {
  return generateItemId("l");
}

export function generateConditionId(): string {
  return generateItemId("c");
}

export function isTestLoop(item: TestScriptItem): item is TestLoop {
  return item.type === "loop";
}

export function isTestUnit(item: TestScriptItem): item is TestLoop & {
  params: TestLoop["params"] & { unit: { name: string } };
} {
  return isTestLoop(item) && typeof item.params.unit?.name === "string";
}

export function isNestableTestItem(item: TestScriptItem): item is TestNestedItem {
  return !isTestCondition(item) && (!isTestLoop(item) || isTestUnit(item));
}

export function isTestCondition(item: TestScriptItem): item is TestCondition {
  return item.type === "condition";
}

function conditionCheckHasAssertion(step: TestStep): boolean {
  if (!step.assert) return false;
  return Object.entries(step.assert).some(([key, value]) => (
    key !== "continue_on_error" && value != null
  ));
}

function serialPattern(params: unknown): string {
  if (params === null || typeof params !== "object") return "";
  const { pattern } = params as { pattern?: unknown };
  return typeof pattern === "string" ? pattern.trim() : "";
}

function assertSerialWaitPattern(step: TestStep, location: string) {
  if (step.type === "serial_wait" && serialPattern(step.params) === "") {
    throw new Error(`${location}: serial_wait pattern must be non-empty`);
  }
}

function assertConditionCheck(step: TestStep, location: string) {
  if (!(["serial_expect", "adc_read", "gpio_assert"] as StepType[]).includes(step.type)) {
    throw new Error(`${location}: unsupported check type "${step.type}"`);
  }
  if (step.type === "adc_read" && !conditionCheckHasAssertion(step)) {
    throw new Error(`${location}: adc_read check requires an assert`);
  }
  if (
    step.type === "serial_expect"
    && serialPattern(step.params) === ""
  ) {
    throw new Error(`${location}: serial_expect check pattern must be non-empty`);
  }
}

/**
 * Inserts a primitive step or named Unit into a loop body or condition branch
 * without mutating the workflow. When sourceItemId is provided, the item is
 * moved from the top-level workflow; otherwise the supplied item is copied in.
 */
export function nestItemInScript(
  items: TestScriptItem[],
  item: TestNestedItem,
  containerId: string,
  branch: TestNestedBranch,
  sourceItemId?: string,
): TestScriptItem[] | null {
  if (!isNestableTestItem(item)) return null;

  const next = [...items];
  let nestedItem: TestNestedItem = item;
  if (sourceItemId) {
    const sourceIndex = next.findIndex((item) => item.id === sourceItemId);
    if (sourceIndex < 0) return null;
    const source = next[sourceIndex];
    if (!isNestableTestItem(source)) return null;
    nestedItem = source;
    next.splice(sourceIndex, 1);
  }

  const containerIndex = next.findIndex((item) => item.id === containerId);
  if (containerIndex < 0) return null;
  const container = next[containerIndex];

  if (isTestCondition(container) && branch !== "body") {
    const key = branch === "then" ? "then_steps" : "else_steps";
    if (container.params[key].some((item) => item.id === nestedItem.id)) return null;
    next[containerIndex] = {
      ...container,
      params: {
        ...container.params,
        [key]: [...container.params[key], nestedItem],
      },
    };
    return next;
  }

  if (isTestLoop(container) && !isTestUnit(container) && branch === "body") {
    if (container.params.steps.some((item) => item.id === nestedItem.id)) return null;
    next[containerIndex] = {
      ...container,
      params: {
        ...container.params,
        steps: [...container.params.steps, nestedItem],
      },
    };
    return next;
  }

  return null;
}

/** Removes one nested item from a loop/Unit body or condition branch. */
export function removeNestedItemFromScript(
  items: TestScriptItem[],
  containerId: string,
  branch: TestNestedBranch,
  nestedItemId: string,
): TestScriptItem[] | null {
  const containerIndex = items.findIndex((item) => item.id === containerId);
  if (containerIndex < 0) return null;
  const container = items[containerIndex];
  const next = [...items];

  if (isTestCondition(container) && branch !== "body") {
    const key = branch === "then" ? "then_steps" : "else_steps";
    const nestedItems = container.params[key];
    if (!nestedItems.some((item) => item.id === nestedItemId)) return null;
    next[containerIndex] = {
      ...container,
      params: {
        ...container.params,
        [key]: nestedItems.filter((item) => item.id !== nestedItemId),
      },
    };
    return next;
  }

  if (isTestLoop(container) && branch === "body") {
    if (!container.params.steps.some((item) => item.id === nestedItemId)) return null;
    next[containerIndex] = {
      ...container,
      params: {
        ...container.params,
        steps: container.params.steps.filter((item) => item.id !== nestedItemId),
      },
    };
    return next;
  }

  return null;
}

/** @deprecated Use nestItemInScript for primitive steps and named Units. */
export function nestUnitInScript(
  items: TestScriptItem[],
  unit: TestLoop,
  containerId: string,
  branch: TestNestedBranch,
  sourceItemId?: string,
): TestScriptItem[] | null {
  return nestItemInScript(items, unit, containerId, branch, sourceItemId);
}

export function countScriptCommands(script: TestScript): number {
  const countNested = (item: TestNestedItem): number => (
    isTestLoop(item)
      ? item.params.steps.reduce((total, child) => total + countNested(child), 0)
      : 1
  );
  return script.steps.reduce((total, item) => {
    if (isTestLoop(item)) {
      return total + item.params.steps.reduce((sum, child) => sum + countNested(child), 0);
    }
    if (isTestCondition(item)) {
      return total
        + 1
        + item.params.then_steps.reduce((sum, child) => sum + countNested(child), 0)
        + item.params.else_steps.reduce((sum, child) => sum + countNested(child), 0);
    }
    return total + 1;
  }, 0);
}

export function buildExecutionPlan(script: TestScript): ExecutionStep[] {
  const plan: ExecutionStep[] = [];
  const itemIds = new Set<string>();
  const executionIds = new Set<string>();

  const appendStep = (step: ExecutionStep) => {
    if (plan.length >= MAX_EXECUTION_STEPS) {
      throw new Error(`Expanded test exceeds ${MAX_EXECUTION_STEPS} executable steps`);
    }
    if (executionIds.has(step.executionId)) {
      throw new Error(`Duplicate execution step ID: ${step.executionId}`);
    }
    executionIds.add(step.executionId);
    plan.push(step);
  };

  type ExpansionContext = Pick<
    ExecutionStep,
    "loopId" | "loopIteration" | "loopCount" | "unitId" | "unitName" | "conditionId" | "conditionRole"
  > & { suffix: string };

  const appendPrimitive = (step: TestStep, context: ExpansionContext) => {
    appendStep({
      ...step,
      executionId: `${step.id}${context.suffix}`,
      sourceStepId: step.id,
      loopId: context.loopId,
      loopIteration: context.loopIteration,
      loopCount: context.loopCount,
      unitId: context.unitId,
      unitName: context.unitName,
      conditionId: context.conditionId,
      conditionRole: context.conditionRole,
    });
  };

  const assertUniqueChildren = (items: TestNestedItem[], location: string) => {
    const childIds = new Set<string>();
    for (const child of items) {
      if (childIds.has(child.id)) throw new Error(`${location}: duplicate child step ID: ${child.id}`);
      childIds.add(child.id);
    }
  };

  const expandUnit = (unit: TestLoop, context: ExpansionContext) => {
    if (!isTestUnit(unit)) throw new Error(`Nested loop ${unit.id}: only named Units may be nested`);
    if (unit.params.count !== 1) throw new Error(`Unit ${unit.id}: count must be exactly 1`);
    if (unit.params.steps.length === 0) throw new Error(`Unit ${unit.id}: at least one step is required`);
    assertUniqueChildren(unit.params.steps, `Unit ${unit.id}`);
    for (const child of unit.params.steps) {
      if (isTestLoop(child)) throw new Error(`Unit ${unit.id}: nested Units are not supported`);
      assertSerialWaitPattern(child, `Unit ${unit.id}`);
      appendPrimitive(child, {
        ...context,
        suffix: `@${unit.id}:1${context.suffix}`,
        unitId: unit.id,
        unitName: unit.params.unit.name,
        loopId: context.loopId ?? unit.id,
        loopIteration: context.loopIteration ?? 1,
        loopCount: context.loopCount ?? 1,
      });
    }
  };

  const expandNested = (item: TestNestedItem, context: ExpansionContext) => {
    if (isTestLoop(item)) expandUnit(item, context);
    else appendPrimitive(item, context);
  };

  for (const item of script.steps) {
    if (itemIds.has(item.id)) {
      throw new Error(`Duplicate script item ID: ${item.id}`);
    }
    itemIds.add(item.id);

    if (isTestCondition(item)) {
      assertConditionCheck(item.params.check, `Condition ${item.id}`);
      assertUniqueChildren(
        [item.params.check, ...item.params.then_steps, ...item.params.else_steps],
        `Condition ${item.id}`,
      );
      assertSerialWaitPattern(item.params.check, `Condition ${item.id} check`);
      appendPrimitive(item.params.check, {
        suffix: `@${item.id}:check`,
        conditionId: item.id,
        conditionRole: "check",
      });
      for (const child of item.params.then_steps) {
        if (!isTestLoop(child)) assertSerialWaitPattern(child, `Condition ${item.id} then`);
        expandNested(child, {
          suffix: `@${item.id}:then`,
          conditionId: item.id,
          conditionRole: "then",
        });
      }
      for (const child of item.params.else_steps) {
        if (!isTestLoop(child)) assertSerialWaitPattern(child, `Condition ${item.id} else`);
        expandNested(child, {
          suffix: `@${item.id}:else`,
          conditionId: item.id,
          conditionRole: "else",
        });
      }
      continue;
    }

    if (!isTestLoop(item)) {
      assertSerialWaitPattern(item, `Step ${item.id}`);
      appendPrimitive(item, { suffix: "" });
      continue;
    }

    const count = item.params.count;
    if (!Number.isInteger(count) || count < MIN_LOOP_COUNT || count > MAX_LOOP_COUNT) {
      throw new Error(
        `Loop ${item.id}: count must be an integer between ${MIN_LOOP_COUNT} and ${MAX_LOOP_COUNT}`,
      );
    }
    if (item.params.steps.length === 0) {
      throw new Error(`Loop ${item.id}: at least one step is required`);
    }
    if (isTestUnit(item) && count !== 1) {
      throw new Error(`Unit ${item.id}: count must be exactly 1`);
    }
    assertUniqueChildren(item.params.steps, `${isTestUnit(item) ? "Unit" : "Loop"} ${item.id}`);

    if (isTestUnit(item)) {
      expandUnit(item, { suffix: "" });
      continue;
    }

    for (let iteration = 1; iteration <= count; iteration += 1) {
      for (const child of item.params.steps) {
        if (!isTestLoop(child)) assertSerialWaitPattern(child, `Loop ${item.id}`);
        expandNested(child, {
          suffix: `@${item.id}:${iteration}`,
          loopId: item.id,
          loopIteration: iteration,
          loopCount: count,
        });
      }
    }
  }

  if (plan.length > MAX_EXECUTION_STEPS) {
    throw new Error(`Expanded test exceeds ${MAX_EXECUTION_STEPS} executable steps`);
  }
  return plan;
}

/**
 * Build an execution plan without throwing through an interactive editor render.
 *
 * Empty loops and Units are useful transient states while a user is composing a
 * workflow. Callers that execute, import, or validate a script must continue to
 * use buildExecutionPlan so incomplete workflows remain strictly rejected.
 */
export function tryBuildExecutionPlan(script: TestScript): ExecutionPlanAttempt {
  try {
    return { plan: buildExecutionPlan(script), error: null };
  } catch (error) {
    return {
      plan: [],
      error: error instanceof Error ? error.message : String(error),
    };
  }
}

export function defaultStepParams<T extends StepType>(type: T): StepParamsFor<T> {
  const params: Record<StepType, Record<string, unknown>> = {
    power_on: { rail: "5v_out" },
    power_off: { rail: "5v_out" },
    delay: { ms: 1000 },
    serial_wait: { channel: "uart0", pattern: "login:", timeout_ms: 60000 },
    serial_send: { channel: "uart0", text: "root\n" },
    serial_expect: { channel: "uart0", command: "uname -a", pattern: "Linux", timeout_ms: 10000 },
    adc_read: { channel: "5v_out" },
    gpio_set: { pin: "GP13", value: 1 },
    gpio_assert: { pin: "GP13", direction: "output", value: 1 },
    switch_route: { switch: "sd", route: "target" },
    capture: { rail: "5v_out", trigger: "manual", duration_ms: 5000, threshold_a: 0.1 },
  };
  return params[type] as unknown as StepParamsFor<T>;
}

export function parseTestScript(
  ndjson: string,
  options: { validatePlan?: boolean } = {},
): TestScript {
  const validatePlan = options.validatePlan !== false;
  const lines = ndjson.split("\n").filter((l) => l.trim());
  if (lines.length === 0) throw new Error("Empty script");
  const header = JSON.parse(lines[0]);
  if (header.schema !== "linkr-test.v1") throw new Error(`Unknown schema: ${header.schema}`);
  const steps: TestScriptItem[] = [];
  const parseChildStep = (child: unknown, location: string): TestStep => {
    if (!child || typeof child !== "object" || Array.isArray(child)) {
      throw new Error(`${location} must be an object`);
    }
    const value = child as Record<string, unknown>;
    if (!value.id || typeof value.id !== "string") {
      throw new Error(`${location} has an invalid "id"`);
    }
    if (!STEP_TYPES.includes(value.type as StepType)) {
      throw new Error(`${location} has unknown step type "${String(value.type)}"`);
    }
    if (value.params != null && (typeof value.params !== "object" || Array.isArray(value.params))) {
      throw new Error(`${location} "params" must be an object`);
    }
    const type = value.type as StepType;
    const step: TestStep = {
      id: value.id,
      type,
      params: {
        ...defaultStepParams(type),
        ...((value.params as Record<string, unknown> | undefined) ?? {}),
      } as StepParamsFor<StepType>,
      assert: value.assert as StepAssertion | undefined,
      continue_on_error: value.continue_on_error as boolean | undefined,
    };
    if (validatePlan) assertSerialWaitPattern(step, location);
    return step;
  };

  const parseNestedUnit = (child: unknown, location: string): TestLoop => {
    if (!child || typeof child !== "object" || Array.isArray(child)) {
      throw new Error(`${location} must be an object`);
    }
    const value = child as Record<string, unknown>;
    const params = value.params as Record<string, unknown> | undefined;
    const unit = params?.unit as Record<string, unknown> | undefined;
    const unitSteps = params?.steps;
    if (value.type !== "loop") throw new Error(`${location} must be a Unit`);
    if (!value.id || typeof value.id !== "string") throw new Error(`${location} has an invalid "id"`);
    if (!unit) throw new Error(`${location}: only named Units may be nested`);
    if (params?.count !== 1) throw new Error(`${location} Unit "count" must be exactly 1`);
    if (
      typeof unit.name !== "string" || unit.name.trim().length === 0 || unit.name.length > 80
    ) {
      throw new Error(`${location} Unit "name" must be a non-empty string up to 80 characters`);
    }
    if (!Array.isArray(unitSteps)) {
      throw new Error(`${location} Unit "steps" must be an array`);
    }
    if (validatePlan && unitSteps.length === 0) {
      throw new Error(`${location} Unit "steps" must be a non-empty array`);
    }
    return {
      id: value.id,
      type: "loop",
      params: {
        count: 1,
        unit: { name: unit.name.trim() },
        steps: unitSteps.map((unitStep: unknown, index: number) => {
          const nestedType = (unitStep as { type?: string } | null)?.type;
          if (nestedType === "loop" || nestedType === "condition") {
            throw new Error(`${location} Unit cannot contain nested flow blocks`);
          }
          return parseChildStep(unitStep, `${location} Unit step ${index + 1}`);
        }),
      },
    };
  };

  const parseNestedItem = (child: unknown, location: string): TestNestedItem => {
    const nestedType = (child as { type?: string } | null)?.type;
    if (nestedType === "loop") return parseNestedUnit(child, location);
    if (nestedType === "condition") throw new Error(`${location}: nested conditions are not supported`);
    return parseChildStep(child, location);
  };

  for (let i = 1; i < lines.length; i++) {
    const obj = JSON.parse(lines[i]);
    if (!obj.id || typeof obj.id !== "string") throw new Error(`Line ${i + 1}: missing or invalid "id"`);
    if (obj.params != null && (typeof obj.params !== "object" || Array.isArray(obj.params))) {
      throw new Error(`Line ${i + 1}: "params" must be an object`);
    }
    if (obj.type === "loop") {
      const count = obj.params?.count;
      const childSteps = obj.params?.steps;
      const unit = obj.params?.unit;
      if (!Number.isInteger(count) || count < MIN_LOOP_COUNT || count > MAX_LOOP_COUNT) {
        throw new Error(
          `Line ${i + 1}: loop "count" must be an integer between ${MIN_LOOP_COUNT} and ${MAX_LOOP_COUNT}`,
        );
      }
      if (!Array.isArray(childSteps)) {
        throw new Error(`Line ${i + 1}: loop "steps" must be an array`);
      }
      if (validatePlan && childSteps.length === 0) {
        throw new Error(`Line ${i + 1}: loop "steps" must be a non-empty array`);
      }
      if (unit != null && (
        typeof unit !== "object"
        || Array.isArray(unit)
        || typeof unit.name !== "string"
        || unit.name.trim().length === 0
        || unit.name.length > 80
      )) {
        throw new Error(`Line ${i + 1}: unit "name" must be a non-empty string up to 80 characters`);
      }
      if (unit != null && count !== 1) {
        throw new Error(`Line ${i + 1}: unit "count" must be exactly 1`);
      }
      const parsedChildren = childSteps.map((child, childIndex) => (
        parseNestedItem(child, `Line ${i + 1}: loop step ${childIndex + 1}`)
      ));
      if (unit != null && parsedChildren.some(isTestLoop)) {
        throw new Error(`Line ${i + 1}: Unit cannot contain nested Units`);
      }
      steps.push({
        id: obj.id,
        type: "loop",
        params: {
          count,
          steps: parsedChildren,
          ...(unit == null ? {} : { unit: { name: unit.name.trim() } }),
        },
      });
      continue;
    }
    if (obj.type === "condition") {
      const check = parseChildStep(obj.params?.check, `Line ${i + 1}: condition check`);
      if (validatePlan) assertConditionCheck(check, `Line ${i + 1}: condition check`);
      else if (!(["serial_expect", "adc_read", "gpio_assert"] as StepType[]).includes(check.type)) {
        throw new Error(`Line ${i + 1}: unsupported condition check type "${check.type}"`);
      }
      if (!Array.isArray(obj.params?.then_steps) || !Array.isArray(obj.params?.else_steps)) {
        throw new Error(`Line ${i + 1}: condition branches must be arrays`);
      }
      steps.push({
        id: obj.id,
        type: "condition",
        params: {
          check: check as TestStep<ConditionCheckType>,
          then_steps: obj.params.then_steps.map((child: unknown, childIndex: number) => (
            parseNestedItem(child, `Line ${i + 1}: then step ${childIndex + 1}`)
          )),
          else_steps: obj.params.else_steps.map((child: unknown, childIndex: number) => (
            parseNestedItem(child, `Line ${i + 1}: else step ${childIndex + 1}`)
          )),
        },
      });
      continue;
    }
    if (!STEP_TYPES.includes(obj.type)) throw new Error(`Line ${i + 1}: unknown step type "${obj.type}"`);
    const type = obj.type as StepType;
    const step: TestStep = {
      id: obj.id,
      type,
      params: { ...defaultStepParams(type), ...(obj.params ?? {}) } as StepParamsFor<StepType>,
      assert: obj.assert,
      continue_on_error: obj.continue_on_error,
    };
    if (validatePlan) assertSerialWaitPattern(step, `Line ${i + 1}`);
    steps.push(step);
  }
  const script: TestScript = {
    schema: "linkr-test.v1",
    name: header.name ?? "Untitled",
    version: header.version ?? "1.0",
    board: header.board,
    created: header.created,
    steps,
  };
  if (validatePlan) buildExecutionPlan(script);
  return script;
}

export function serializeTestScript(script: TestScript): string {
  const header = {
    schema: script.schema,
    name: script.name,
    version: script.version,
    board: script.board,
    created: script.created ?? new Date().toISOString(),
  };
  const lines = [JSON.stringify(header)];
  for (const step of script.steps) {
    const obj: Record<string, unknown> = { id: step.id, type: step.type, params: step.params };
    if (!isTestLoop(step) && !isTestCondition(step) && step.assert) obj.assert = step.assert;
    if (!isTestLoop(step) && !isTestCondition(step) && step.continue_on_error) {
      obj.continue_on_error = step.continue_on_error;
    }
    lines.push(JSON.stringify(obj));
  }
  return lines.join("\n") + "\n";
}

export function defaultScript(): TestScript {
  return {
    schema: "linkr-test.v1",
    name: "New Test",
    version: "1.0",
    steps: [],
  };
}
import type { PowerCapture } from "./types.ts";
