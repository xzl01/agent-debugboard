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
  readPower: (name: string) => Promise<{ state: string; currentUa: number }>;
  setSwitch: (name: string, route: string) => Promise<void>;
  setGpio: (identifier: string, direction: "input" | "output", value?: number) => Promise<void>;
  enterBootloader: () => Promise<void>;
  captureState: BoardCaptureState;
  captureProgress: BoardCaptureProgress | null;
  captures: PowerCapture[];
  armCapture: (config: CaptureConfig) => Promise<void>;
  triggerCapture: () => void;
  stopCapture: () => void;
  cancelCapture: () => void;
  clearCaptures: () => void;
}
