import type {
  ConditionCheckType,
  StepType,
  TestCondition,
  TestLoop,
  TestNestedItem,
  TestNestedBranch,
  TestScript,
  TestScriptItem,
  TestStep,
} from "@/lib/testScript";

// ─── Drag & Drop ───────────────────────────────────────────────────────────────

export const DRAG_MIME = "application/x-linkr-workflow-item";

export type DragPayload =
  | { kind: "palette"; stepType: StepType }
  | { kind: "unit"; templateId: string }
  | { kind: "item"; itemId: string };

export type NestedDropBranch = TestNestedBranch;

/** Consolidated drag state managed by a single reducer. */
export interface DragState {
  active: boolean;
  kind: DragPayload["kind"] | null;
  payload: DragPayload | null;
  nestable: boolean;
  itemId: string | null;
  overIndex: number | null;
  nestedTarget: string | null;
}

export const DRAG_INITIAL: DragState = {
  active: false,
  kind: null,
  payload: null,
  nestable: false,
  itemId: null,
  overIndex: null,
  nestedTarget: null,
};

export type DragAction =
  | { type: "start"; payload: DragPayload; nestable?: boolean }
  | { type: "over"; index: number }
  | { type: "nested"; target: string }
  | { type: "end" };

export function dragReducer(state: DragState, action: DragAction): DragState {
  switch (action.type) {
    case "start":
      return {
        active: true,
        kind: action.payload.kind,
        payload: action.payload,
        nestable: action.nestable ?? action.payload.kind !== "item",
        itemId: action.payload.kind === "item" ? action.payload.itemId : null,
        overIndex: null,
        nestedTarget: null,
      };
    case "over":
      return { ...state, overIndex: action.index, nestedTarget: null };
    case "nested":
      return { ...state, nestedTarget: action.target, overIndex: null };
    case "end":
      return DRAG_INITIAL;
    default:
      return state;
  }
}

// ─── Custom Units ──────────────────────────────────────────────────────────────

export interface CustomUnitTemplate {
  id: string;
  name: string;
  steps: TestStep[];
}

export const CUSTOM_UNITS_KEY = "linkr-test-custom-units.v1";

// ─── Shared UI constants ───────────────────────────────────────────────────────

export const inputClass =
  "min-h-10 w-full rounded-xl border border-line/70 bg-panel px-3 py-2 text-sm text-ink outline-none transition-colors focus-visible:border-brand/60 focus-visible:ring-2 focus-visible:ring-brand/20";

export const MAX_NESTED_PREVIEW_STEPS = 4;

// ─── Component prop types ──────────────────────────────────────────────────────

export interface WorkflowComposerProps {
  script: TestScript;
  onChange: (script: TestScript) => void;
  onRun: () => void;
  runDisabled?: boolean;
}

// Re-export for convenience
export type {
  ConditionCheckType,
  StepType,
  TestCondition,
  TestLoop,
  TestNestedItem,
  TestScript,
  TestScriptItem,
  TestStep,
};
