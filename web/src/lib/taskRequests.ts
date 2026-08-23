import {
  buildExecutionPlan,
  type DelayParams,
  type ExecutionStep,
  type GpioSetParams,
  type PowerOffParams,
  type PowerOnParams,
  type SwitchRouteParams,
  type TestScript,
} from "./testScript.ts";

/**
 * Task persistence: a task blob is a sequence of raw HTTP request records,
 * one NDJSON line each, stored by the firmware and executed by clients
 * against the same paths the live API serves. `wait_ms` is task metadata
 * applied client-side after that request; it is not part of the public API.
 */
export interface TaskRequest {
  method: "PUT";
  path: string;
  body: string;
  wait_ms?: number;
}

// Mirrors LINKR_DEBUGGER_TASK_MAX_REQUESTS / LINKR_DEBUGGER_TASK_MAX_DELAY_MS.
export const TASK_MAX_REQUESTS = 32;
export const TASK_MAX_WAIT_MS = 60_000;

const API_PREFIX = "/api/v1";

function stepToRequest(step: ExecutionStep): TaskRequest | null {
  switch (step.type) {
    case "power_on":
    case "power_off": {
      const params = step.params as PowerOnParams | PowerOffParams;
      return {
        method: "PUT",
        path: `${API_PREFIX}/power/${encodeURIComponent(params.rail)}`,
        body: JSON.stringify({ state: step.type === "power_on" ? "on" : "off" }),
      };
    }
    case "gpio_set": {
      const { pin, direction, value } = step.params as GpioSetParams;
      if (direction == null) {
        throw new Error(
          `Step ${step.sourceStepId} gpio_set on ${pin} has no direction and cannot be stored as a task request`,
        );
      }
      if (direction === "output" && value == null) {
        throw new Error(
          `Step ${step.sourceStepId} gpio_set on ${pin} is an output without a value and cannot be stored as a task request`,
        );
      }
      return {
        method: "PUT",
        path: `${API_PREFIX}/gpio/${encodeURIComponent(pin)}`,
        body: JSON.stringify(
          direction === "output" ? { direction, value } : { direction },
        ),
      };
    }
    case "switch_route": {
      const params = step.params as SwitchRouteParams;
      return {
        method: "PUT",
        path: `${API_PREFIX}/switch/${params.switch}`,
        body: JSON.stringify({ route: params.route }),
      };
    }
    case "delay":
      return null;
    default:
      throw new Error(
        `Step ${step.sourceStepId} uses ${step.type}, which cannot be stored as a task request`,
      );
  }
}

/**
 * Expands a workflow and maps every supported step to a raw HTTP request
 * record. Delay steps merge into the preceding request's `wait_ms`; any other
 * unsupported step type rejects the whole script.
 */
export function buildTaskRequests(script: TestScript): TaskRequest[] {
  const plan = buildExecutionPlan(script);
  const requests: TaskRequest[] = [];
  for (const step of plan) {
    if (step.type === "delay") {
      const previous = requests.at(-1);
      if (!previous) {
        throw new Error(
          `Step ${step.sourceStepId} is a delay with no preceding request and cannot be stored as a task request`,
        );
      }
      const waitMs = (previous.wait_ms ?? 0) + (step.params as DelayParams).ms;
      if (waitMs > TASK_MAX_WAIT_MS) {
        throw new Error(
          `Step ${step.sourceStepId} raises the wait after ${previous.path} to ${waitMs} ms, above the firmware limit of ${TASK_MAX_WAIT_MS} ms`,
        );
      }
      previous.wait_ms = waitMs;
      continue;
    }
    const request = stepToRequest(step);
    if (request) requests.push(request);
  }
  if (requests.length > TASK_MAX_REQUESTS) {
    throw new Error(
      `Script expands to ${requests.length} requests, above the firmware limit of ${TASK_MAX_REQUESTS} per task`,
    );
  }
  return requests;
}

export function serializeTaskRequests(requests: readonly TaskRequest[]): string {
  return requests.map((request) => JSON.stringify(request)).join("\n") + "\n";
}

export function buildTaskBlob(taskId: string, script: TestScript): string {
  return `# linkr-task.v1\n# task ${taskId}\n${serializeTaskRequests(buildTaskRequests(script))}`;
}
