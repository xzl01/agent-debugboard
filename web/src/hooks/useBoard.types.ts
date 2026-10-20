import type {
  BoardCaptureProgress,
  BoardCaptureState,
  BoardSnapshot,
  CaptureConfig,
  PowerCapture,
} from "@/lib/types";

export interface UseBoard {
  snapshot: BoardSnapshot;
  persistentConfigCurrentStateKey: string;
  hasData: boolean;
  connected: boolean;
  error: string | null;
  loading: boolean;
  auto: boolean;
  setAuto: (v: boolean) => void;
  live: boolean;
  setLive: (v: boolean) => void;
  refresh: () => Promise<void>;
  setPower: (name: string, on: boolean) => Promise<void>;
  setPd: (mode: 0 | 1, voltageMv: number) => Promise<void>;
  readPower: (name: string) => Promise<{ state: string; currentUa: number; voltageUv: number }>;
  setSwitch: (name: string, route: string) => Promise<void>;
  setGpio: (identifier: string, direction: "input" | "output", value?: number) => Promise<void>;
  enterBootloader: () => Promise<void>;
  rebootBoard: () => Promise<void>;
  restarting: boolean;
  restartTimedOut: boolean;
  captureState: BoardCaptureState;
  captureProgress: BoardCaptureProgress | null;
  captures: PowerCapture[];
  armCapture: (config: CaptureConfig) => Promise<void>;
  triggerCapture: () => void;
  stopCapture: () => void;
  cancelCapture: () => void;
  clearCaptures: () => void;
}
