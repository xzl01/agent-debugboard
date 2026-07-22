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

export interface TestScript {
  schema: "linkr-test.v1";
  name: string;
  version: string;
  board?: string;
  created?: string;
  steps: TestStep[];
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
}

export function isRunSuccessful(summary: RunSummary): boolean {
  return summary.completed && !summary.aborted && summary.results.length === summary.totalSteps &&
    summary.results.every((result) => result.status === "pass");
}

export interface AdcSampleEntry {
  stepId: string;
  channel: string;
  currentUa: number;
  timestampMs: number;
}

export interface SerialLogEntry {
  stepId: string;
  text: string;
  direction: "rx" | "tx";
  timestampMs: number;
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

export function generateStepId(): string {
  return `s${crypto.randomUUID().slice(0, 8)}`;
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

export function parseTestScript(ndjson: string): TestScript {
  const lines = ndjson.split("\n").filter((l) => l.trim());
  if (lines.length === 0) throw new Error("Empty script");
  const header = JSON.parse(lines[0]);
  if (header.schema !== "linkr-test.v1") throw new Error(`Unknown schema: ${header.schema}`);
  const steps: TestStep[] = [];
  for (let i = 1; i < lines.length; i++) {
    const obj = JSON.parse(lines[i]);
    if (!obj.id || typeof obj.id !== "string") throw new Error(`Line ${i + 1}: missing or invalid "id"`);
    if (!STEP_TYPES.includes(obj.type)) throw new Error(`Line ${i + 1}: unknown step type "${obj.type}"`);
    const type = obj.type as StepType;
    steps.push({
      id: obj.id,
      type,
      params: { ...defaultStepParams(type), ...(obj.params ?? {}) } as StepParamsFor<StepType>,
      assert: obj.assert,
      continue_on_error: obj.continue_on_error,
    });
  }
  return {
    schema: "linkr-test.v1",
    name: header.name ?? "Untitled",
    version: header.version ?? "1.0",
    board: header.board,
    created: header.created,
    steps,
  };
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
    if (step.assert) obj.assert = step.assert;
    if (step.continue_on_error) obj.continue_on_error = step.continue_on_error;
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
