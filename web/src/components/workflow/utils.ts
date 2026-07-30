import type { DragEvent } from "react";
import {
  Activity,
  CheckCircle2,
  GitBranch,
  Hourglass,
  MessageSquareText,
  Power,
  Send,
  Timer,
  ToggleRight,
  Zap,
  type LucideIcon,
} from "lucide-react";
import type {
  ConditionCheckType,
  CustomUnitTemplate,
  DragPayload,
  StepType,
  TestLoop,
  TestNestedItem,
  TestScriptItem,
  TestStep,
} from "./types";
import { CUSTOM_UNITS_KEY, DRAG_MIME } from "./types";
import {
  defaultStepParams,
  generateConditionId,
  generateLoopId,
  generateStepId,
  isTestCondition,
  isTestLoop,
  isTestUnit,
  STEP_TYPES,
} from "@/lib/testScript";

// ─── Step icons ────────────────────────────────────────────────────────────────

export const STEP_ICONS: Record<StepType, LucideIcon> = {
  power_on: Zap,
  power_off: Power,
  delay: Timer,
  serial_wait: Hourglass,
  serial_send: Send,
  serial_expect: MessageSquareText,
  adc_read: Activity,
  gpio_set: ToggleRight,
  gpio_assert: CheckCircle2,
  switch_route: GitBranch,
  capture: Activity,
};

// ─── Palette config ────────────────────────────────────────────────────────────

export const PALETTE_GROUPS: Array<{ key: string; types: StepType[] }> = [
  { key: "power", types: ["power_on", "power_off", "delay"] },
  { key: "serial", types: ["serial_wait", "serial_send"] },
  { key: "measure", types: ["adc_read", "capture"] },
  { key: "control", types: ["switch_route", "gpio_set"] },
  { key: "assert", types: ["serial_expect", "gpio_assert"] },
];

export const CONDITION_CHECK_TYPES: ConditionCheckType[] = ["serial_expect", "adc_read", "gpio_assert"];

// ─── Drag helpers ──────────────────────────────────────────────────────────────

export function setDragPayload(event: DragEvent, payload: DragPayload) {
  const value = JSON.stringify(payload);
  event.dataTransfer.setData(DRAG_MIME, value);
  event.dataTransfer.setData("text/plain", value);
  event.dataTransfer.effectAllowed = payload.kind === "item" ? "move" : "copy";
  const source = (event.currentTarget as HTMLElement).closest<HTMLElement>("[data-workflow-drag-preview]")
    ?? event.currentTarget as HTMLElement;
  const bounds = source.getBoundingClientRect();
  event.dataTransfer.setDragImage(
    source,
    Math.min(44, Math.max(12, bounds.width * 0.08)),
    Math.min(32, Math.max(12, bounds.height * 0.35)),
  );
}

export function getDragPayload(event: DragEvent): DragPayload | null {
  const raw = event.dataTransfer.getData(DRAG_MIME) || event.dataTransfer.getData("text/plain");
  if (!raw) return null;
  try {
    const value = JSON.parse(raw) as Partial<DragPayload>;
    if (value.kind === "palette" && typeof value.stepType === "string") {
      return value as DragPayload;
    }
    if (value.kind === "unit" && typeof value.templateId === "string") {
      return value as DragPayload;
    }
    if (value.kind === "item" && typeof value.itemId === "string") {
      return value as DragPayload;
    }
  } catch {
    // Ignore unrelated browser drags.
  }
  return null;
}

// ─── Custom unit persistence ───────────────────────────────────────────────────

export function loadCustomUnits(): CustomUnitTemplate[] {
  try {
    const saved = localStorage.getItem(CUSTOM_UNITS_KEY);
    if (!saved) return [];
    const parsed = JSON.parse(saved) as unknown;
    if (!Array.isArray(parsed)) return [];
    return parsed.flatMap((entry) => {
      if (!entry || typeof entry !== "object") return [];
      const value = entry as Partial<CustomUnitTemplate>;
      if (typeof value.id !== "string" || typeof value.name !== "string" || !Array.isArray(value.steps)) {
        return [];
      }
      const steps = value.steps.flatMap((step): TestStep[] => {
        if (!step || typeof step !== "object" || Array.isArray(step)) return [];
        const candidate = step as Partial<TestStep>;
        if (
          typeof candidate.id !== "string"
          || !STEP_TYPES.includes(candidate.type as StepType)
          || candidate.params == null
          || typeof candidate.params !== "object"
          || Array.isArray(candidate.params)
        ) {
          return [];
        }
        const type = candidate.type as StepType;
        return [{
          id: candidate.id,
          type,
          params: {
            ...defaultStepParams(type),
            ...(candidate.params as unknown as Record<string, unknown>),
          } as TestStep["params"],
          assert: candidate.assert,
          continue_on_error: candidate.continue_on_error,
        }];
      });
      return steps.length > 0 ? [{ id: value.id, name: value.name, steps }] : [];
    });
  } catch {
    return [];
  }
}

// ─── Clone helpers ─────────────────────────────────────────────────────────────

export function cloneStep(step: TestStep): TestStep {
  return {
    ...step,
    id: generateStepId(),
    params: { ...step.params },
    assert: step.assert ? { ...step.assert } : undefined,
  };
}

export function cloneNestedItem(item: TestNestedItem): TestNestedItem {
  if (!isTestLoop(item)) return cloneStep(item);
  return {
    ...item,
    id: generateLoopId(),
    params: {
      ...item.params,
      steps: item.params.steps.map(cloneNestedItem),
      unit: item.params.unit ? { ...item.params.unit } : undefined,
    },
  };
}

export function cloneItem(item: TestScriptItem): TestScriptItem {
  if (isTestCondition(item)) {
    return {
      ...item,
      id: generateConditionId(),
      params: {
        check: cloneStep(item.params.check) as TestStep<ConditionCheckType>,
        then_steps: item.params.then_steps.map(cloneNestedItem),
        else_steps: item.params.else_steps.map(cloneNestedItem),
      },
    };
  }
  if (!isTestLoop(item)) return cloneStep(item);
  return {
    ...item,
    id: generateLoopId(),
    params: {
      ...item.params,
      steps: item.params.steps.map(cloneNestedItem),
      unit: item.params.unit ? { ...item.params.unit } : undefined,
    },
  };
}

// ─── Factory helpers ───────────────────────────────────────────────────────────

export function unitFromTemplate(template: CustomUnitTemplate): TestLoop {
  return {
    id: generateLoopId(),
    type: "loop",
    params: {
      count: 1,
      steps: template.steps.map(cloneStep),
      unit: { name: template.name },
    },
  };
}

export function createConditionCheck(type: ConditionCheckType): TestStep<ConditionCheckType> {
  const step: TestStep<ConditionCheckType> = {
    id: generateStepId(),
    type,
    params: defaultStepParams(type),
  };
  if (type === "adc_read") {
    step.assert = { current_range: { min_a: 0, max_a: 3 } };
  }
  return step;
}

/**
 * Flatten group-selected items into primitive steps for creating a new group.
 * Conditions are explicitly excluded (guarded by groupSelectionIsValid upstream).
 */
export function flattenGroupItems(items: TestScriptItem[]): TestStep[] {
  return items.flatMap((item) => {
    if (isTestCondition(item)) return []; // defensive: conditions cannot be grouped
    if (isTestUnit(item)) {
      return item.params.steps.flatMap((child) => isTestLoop(child) ? [] : [cloneStep(child)]);
    }
    if (isTestLoop(item)) return []; // defensive: bare loops cannot be grouped
    return [cloneStep(item)];
  });
}
