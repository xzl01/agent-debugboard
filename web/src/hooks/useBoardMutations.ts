import { useCallback } from "react";
import * as api from "@/lib/api";
import {
  isCurrentAdcReading,
  parseHttpAdcReadings,
} from "@/lib/adc";

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

export interface UseBoardMutationsOptions {
  readonly live: boolean;
  readonly refresh: () => Promise<void>;
}

export interface UseBoardMutations {
  readonly setPower: (name: string, on: boolean) => Promise<void>;
  readonly readPower: (name: string) => Promise<{ state: string; currentUa: number }>;
  readonly setSwitch: (name: string, route: string) => Promise<void>;
  readonly setGpio: (identifier: string, direction: "input" | "output", value?: number) => Promise<void>;
  readonly enterBootloader: () => Promise<void>;
}

export function useBoardMutations(options: UseBoardMutationsOptions): UseBoardMutations {
  const { live, refresh } = options;

  const setPower = useCallback(
    async (name: string, on: boolean) => {
      const response = await api.setPower(name, on);
      const expectedState = on ? "on" : "off";
      if (response?.power_output?.name !== name || response?.power_output?.state !== expectedState) {
        throw new Error(`Power output ${name} did not confirm state ${expectedState}`);
      }
      if (!live) await refresh();
    },
    [live, refresh]
  );

  const readPower = useCallback(async (name: string) => {
    // Keep these reads sequential. The firmware HTTP server has a deliberately
    // small client pool and the live WebSocket already owns one slot; opening
    // two more requests at once can cause a transient connection refusal
    // exactly while a power-cycle task is verifying the shutdown edge.
    const statusResponse: unknown = await api.getStatus();
    const adcResponse: unknown = await api.getAdc();
    const status = isRecord(statusResponse) ? statusResponse : {};
    const output = Array.isArray(status.power_outputs)
      ? status.power_outputs.filter(isRecord).find((item) => item.name === name)
      : undefined;
    const reading = parseHttpAdcReadings(adcResponse)
      .filter(isCurrentAdcReading)
      .find((item) => item.name === name);
    if (!output) throw new Error(`Power output ${name} was not reported by the device`);
    return {
      state: typeof output.state === "string" ? output.state : "",
      currentUa: Math.max(0, reading?.value ?? 0),
    };
  }, []);

  const setSwitch = useCallback(
    async (name: string, route: string) => {
      await api.setSwitch(name, route);
      if (!live) await refresh();
    },
    [live, refresh]
  );

  const setGpio = useCallback(
    async (identifier: string, direction: "input" | "output", value?: number) => {
      await api.setGpio(identifier, direction, value);
      if (!live) await refresh();
    },
    [live, refresh]
  );

  const enterBootloader = useCallback(async () => {
    await api.enterBootloader();
  }, []);

  return { setPower, readPower, setSwitch, setGpio, enterBootloader };
}
