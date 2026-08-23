import { describe, expect, it } from "vitest";
import {
  buildTaskBlob,
  buildTaskRequests,
  TASK_MAX_WAIT_MS,
  type TaskRequest,
} from "./taskRequests";
import type { TestScript, TestScriptItem } from "./testScript";

function scriptWith(steps: TestScriptItem[]): TestScript {
  return { schema: "linkr-test.v1", name: "test", version: "1.0", steps };
}

function storableScript(): TestScript {
  return scriptWith([
    { id: "input", type: "gpio_set", params: { pin: "GP13", direction: "input" } },
    { id: "power_off", type: "power_off", params: { rail: "5v_out" } },
    { id: "wait_off", type: "delay", params: { ms: 1000 } },
    { id: "power_on", type: "power_on", params: { rail: "5v_out" } },
  ]);
}

describe("buildTaskRequests", () => {
  it("maps ordinary GPIO and power steps to raw HTTP requests with post-request waits", () => {
    const requests = buildTaskRequests(storableScript());
    expect(requests).toEqual<TaskRequest[]>([
      { method: "PUT", path: "/api/v1/gpio/GP13", body: '{"direction":"input"}' },
      { method: "PUT", path: "/api/v1/power/5v_out", body: '{"state":"off"}', wait_ms: 1000 },
      { method: "PUT", path: "/api/v1/power/5v_out", body: '{"state":"on"}' },
    ]);
  });

  it("maps switch_route steps to the switch API", () => {
    const requests = buildTaskRequests(
      scriptWith([{ id: "route", type: "switch_route", params: { switch: "sd", route: "target" } }]),
    );
    expect(requests).toEqual([
      { method: "PUT", path: "/api/v1/switch/sd", body: '{"route":"target"}' },
    ]);
  });

  it("expands loops before mapping steps", () => {
    const requests = buildTaskRequests(
      scriptWith([
        {
          id: "loop1",
          type: "loop",
          params: {
            count: 2,
            steps: [
              { id: "on", type: "power_on", params: { rail: "5v_out" } },
              { id: "off", type: "power_off", params: { rail: "5v_out" } },
            ],
          },
        },
      ]),
    );
    expect(requests.map((request) => request.body)).toEqual([
      '{"state":"on"}',
      '{"state":"off"}',
      '{"state":"on"}',
      '{"state":"off"}',
    ]);
  });

  it("accumulates consecutive delays onto the preceding request", () => {
    const requests = buildTaskRequests(
      scriptWith([
        { id: "off", type: "power_off", params: { rail: "5v_out" } },
        { id: "d1", type: "delay", params: { ms: 100 } },
        { id: "d2", type: "delay", params: { ms: 50 } },
      ]),
    );
    expect(requests).toEqual([
      { method: "PUT", path: "/api/v1/power/5v_out", body: '{"state":"off"}', wait_ms: 150 },
    ]);
  });

  it("rejects a delay with no preceding request", () => {
    expect(() =>
      buildTaskRequests(scriptWith([{ id: "d1", type: "delay", params: { ms: 10 } }])),
    ).toThrow(/d1.*delay with no preceding request/);
  });

  it("rejects accumulated waits above the firmware limit", () => {
    expect(() =>
      buildTaskRequests(
        scriptWith([
          { id: "off", type: "power_off", params: { rail: "5v_out" } },
          { id: "d1", type: "delay", params: { ms: TASK_MAX_WAIT_MS + 1 } },
        ]),
      ),
    ).toThrow(/above the firmware limit/);
  });

  it("rejects steps that have no task request mapping", () => {
    expect(() =>
      buildTaskRequests(
        scriptWith([
          { id: "off", type: "power_off", params: { rail: "5v_out" } },
          { id: "adc", type: "adc_read", params: { channel: "5v_out" } },
        ]),
      ),
    ).toThrow(/adc uses adc_read, which cannot be stored as a task request/);
  });

  it("rejects gpio_set output steps without a value", () => {
    expect(() =>
      buildTaskRequests(
        scriptWith([
          { id: "g1", type: "gpio_set", params: { pin: "GP13", direction: "output" } },
        ]),
      ),
    ).toThrow(/g1.*output without a value/);
  });

  it("rejects scripts that expand beyond the firmware request limit", () => {
    expect(() =>
      buildTaskRequests(
        scriptWith([
          {
            id: "loop1",
            type: "loop",
            params: {
              count: 33,
              steps: [{ id: "on", type: "power_on", params: { rail: "5v_out" } }],
            },
          },
        ]),
      ),
    ).toThrow(/above the firmware limit of 32 per task/);
  });
});

describe("buildTaskBlob", () => {
  it("emits the task v1 header followed by one JSON request per line", () => {
    const blob = buildTaskBlob("power-cycle", storableScript());
    const lines = blob.split("\n");
    expect(lines[0]).toBe("# linkr-task.v1");
    expect(lines[1]).toBe("# task power-cycle");
    expect(lines[2]).toBe('{"method":"PUT","path":"/api/v1/gpio/GP13","body":"{\\"direction\\":\\"input\\"}"}');
    expect(lines[3]).toBe(
      '{"method":"PUT","path":"/api/v1/power/5v_out","body":"{\\"state\\":\\"off\\"}","wait_ms":1000}',
    );
    expect(blob.endsWith("\n")).toBe(true);
    expect(lines.filter((line) => line.startsWith("{"))).toHaveLength(3);
  });
});
