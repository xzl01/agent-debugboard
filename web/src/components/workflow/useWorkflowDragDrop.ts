import { useCallback, useReducer, type DragEvent } from "react";
import {
  defaultStepParams,
  generateStepId,
  isNestableTestItem,
  nestItemInScript,
} from "@/lib/testScript";
import type {
  CustomUnitTemplate,
  NestedDropBranch,
  StepType,
  TestNestedItem,
  TestScript,
  TestScriptItem,
} from "./types";
import { DRAG_INITIAL, DRAG_MIME, dragReducer } from "./types";
import { getDragPayload, setDragPayload, unitFromTemplate } from "./utils";

interface WorkflowDragDropOptions {
  script: TestScript;
  customUnits: CustomUnitTemplate[];
  commitSteps: (steps: TestScriptItem[]) => boolean;
  addStep: (type: StepType, index?: number) => void;
  addCustomUnit: (template: CustomUnitTemplate, index?: number) => void;
  selectItem: (id: string) => void;
}

export function useWorkflowDragDrop({
  script,
  customUnits,
  commitSteps,
  addStep,
  addCustomUnit,
  selectItem,
}: WorkflowDragDropOptions) {
  const [drag, dragDispatch] = useReducer(dragReducer, DRAG_INITIAL);

  const handleDrop = useCallback((event: DragEvent<HTMLElement>, insertIndex: number) => {
    event.preventDefault();
    dragDispatch({ type: "end" });
    const payload = getDragPayload(event) ?? drag.payload;
    if (!payload) return;

    if (payload.kind === "palette") {
      addStep(payload.stepType, insertIndex);
      return;
    }
    if (payload.kind === "unit") {
      const template = customUnits.find((unit) => unit.id === payload.templateId);
      if (template) addCustomUnit(template, insertIndex);
      return;
    }

    const fromIndex = script.steps.findIndex((item) => item.id === payload.itemId);
    if (fromIndex < 0) return;
    const steps = [...script.steps];
    const [item] = steps.splice(fromIndex, 1);
    const targetIndex = fromIndex < insertIndex ? insertIndex - 1 : insertIndex;
    steps.splice(Math.max(0, Math.min(targetIndex, steps.length)), 0, item);
    if (commitSteps(steps)) selectItem(item.id);
  }, [addCustomUnit, addStep, commitSteps, customUnits, drag.payload, script.steps, selectItem]);

  const handleNestedItemDrop = useCallback((
    event: DragEvent<HTMLElement>,
    containerId: string,
    branch: NestedDropBranch,
  ) => {
    event.preventDefault();
    event.stopPropagation();
    dragDispatch({ type: "end" });
    const payload = getDragPayload(event) ?? drag.payload;
    if (!payload) return;

    let nestedItem: TestNestedItem | undefined;
    let sourceItemId: string | undefined;
    if (payload.kind === "palette") {
      nestedItem = {
        id: generateStepId(),
        type: payload.stepType,
        params: defaultStepParams(payload.stepType),
      };
    } else if (payload.kind === "unit") {
      const template = customUnits.find((item) => item.id === payload.templateId);
      if (template) nestedItem = unitFromTemplate(template);
    } else {
      const source = script.steps.find((item) => item.id === payload.itemId);
      if (source && isNestableTestItem(source)) {
        nestedItem = source;
        sourceItemId = source.id;
      }
    }
    if (!nestedItem) return;

    const steps = nestItemInScript(script.steps, nestedItem, containerId, branch, sourceItemId);
    if (!steps) return;
    if (commitSteps(steps)) selectItem(containerId);
  }, [commitSteps, customUnits, drag.payload, script.steps, selectItem]);

  const handleNodeDragStart = useCallback((event: DragEvent<HTMLButtonElement>, index: number) => {
    const item = script.steps[index];
    if (!item) return;
    const payload = { kind: "item" as const, itemId: item.id };
    setDragPayload(event, payload);
    dragDispatch({ type: "start", payload, nestable: isNestableTestItem(item) });
  }, [script.steps]);

  const handleCanvasDragEnter = useCallback((event: DragEvent<HTMLElement>) => {
    if (!drag.active && event.dataTransfer.types.includes(DRAG_MIME)) {
      const payload = getDragPayload(event);
      if (payload) dragDispatch({ type: "start", payload });
    }
  }, [drag.active]);

  const handleCanvasDragLeave = useCallback((event: DragEvent<HTMLElement>) => {
    if (!event.currentTarget.contains(event.relatedTarget as Node | null)) {
      dragDispatch({ type: "end" });
    }
  }, []);

  const handleDragEnd = useCallback(() => dragDispatch({ type: "end" }), []);
  const handleNestedDragEnter = useCallback((target: string) => {
    dragDispatch({ type: "nested", target });
  }, []);

  return {
    drag,
    dragDispatch,
    handleDrop,
    handleNestedItemDrop,
    handleNodeDragStart,
    handleCanvasDragEnter,
    handleCanvasDragLeave,
    handleDragEnd,
    handleNestedDragEnter,
  };
}
