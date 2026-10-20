import { useCallback } from "react";
import type { Dispatch, SetStateAction } from "react";
import type { BoardSnapshot } from "@/lib/types";
import { parsePdStatus } from "@/lib/pd";
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
  readonly setSnapshot: Dispatch<SetStateAction<BoardSnapshot>>;
}

export interface UseBoardMutations {
  readonly setPower: (name: string, on: boolean) => Promise<void>;
  readonly readPower: (name: string) => Promise<{ state: string; currentUa: number; voltageUv: number }>;
  readonly setPd: (mode: 0 | 1, voltageMv: number) => Promise<void>;
  readonly setSwitch: (name: string, route: string) => Promise<void>;
  readonly setGpio: (identifier: string, direction: "input" | "output", value?: number) => Promise<void>;
  readonly enterBootloader: () => Promise<void>;
}

export function useBoardMutations(options: UseBoardMutationsOptions): UseBoardMutations {
  const { live, refresh, setSnapshot } = options;

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
      voltageUv: Math.max(0, reading?.voltage_uv ?? 0),
    };
  }, []);

  const setSwitch = useCallback(
    async (name: string, route: string) => {
      await api.setSwitch(name, route);
      if (!live) await refresh();
    },
    [live, refresh]
  );

  const setPd = useCallback(
    async (mode: 0 | 1, voltageMv: number) => {
      const response = await api.setPd(mode, voltageMv);
      setSnapshot((prev) => ({ ...prev, pd: parsePdStatus(response, prev.pd) }));
      if (!live) await refresh();
    },
    [live, refresh, setSnapshot]
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

  return { setPower, readPower, setPd, setSwitch, setGpio, enterBootloader };
}
