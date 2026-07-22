// Types mirror the JSON shapes returned by the Radxa Linkr Debugger firmware
// HTTP API (apps/radxa_linkr_debugger/src/linkr_debugger_http.c) and the
// WebSocket "snapshot"/"telemetry" messages.

export interface Availability {
  available: boolean;
  reason?: string;
  source?: string;
  error?: number;
}

export interface MemoryPressureSnapshot extends Availability {
  coverage?: string;
  pressure_pct_x100?: number;
  limiting_component?: string;
  limiting_name?: string;
  tie_count?: number;
  since?: string;
}

export interface PowerOutput {
  name: string;
  signal?: string;
  gp?: number;
  controllable: boolean;
  state: string; // "on" | "off" | "locked"
  value: number | null;
}

export interface SwitchState {
  sd: string; // "target" | "usb-reader"
  usb: string; // "pc" | "target"
  vin?: string; // "1.8v" | "3.3v"
}

export interface AdcReading {
  name: string;
  signal: string;
  power_enabled: boolean;
  raw: number | null;
  mv: number;
  sensor_channel: string;
  unit: string;
  sensor_value?: { val1: number; val2: number };
  current_ua: number;
}

export type CaptureTrigger = "manual" | "current" | "gpio" | "power_on";

export interface CaptureConfig {
  trigger: CaptureTrigger;
  source: string;
  edge: "rising" | "falling" | "either";
  thresholdUa: number;
  rateHz: number;
  preSamples: number;
  postSamples: number;
}

export interface CaptureSample {
  offset: number;
  triggered: boolean;
  sampleSequence: number;
  deviceTimeUs: number;
  readings: AdcReading[];
}

export interface PowerCapture {
  id: number;
  trigger: string;
  source: string;
  edge: string;
  thresholdUa: number;
  rateHz: number;
  preSamples: number;
  postSamples: number;
  triggerOffset: number;
  samples: CaptureSample[];
  capturedAt: number;
}

export interface SafeGpio {
  name: string;
  pin: number;
  note: string;
  value: number;
  direction: string; // "input" | "output"
  layoutGroup?: string;
  layoutLabel?: string;
  layoutRow?: number;
  layoutColumn?: number;
}

export interface WatchdogStatus {
  supported: boolean;
  automatic: boolean;
  healthy: boolean;
  armed: boolean;
  timeout_ms: number;
  bootloader_on_timeout: boolean;
  failing_service: string;
}

export interface BoardMonitoring {
  temperature: Availability & { celsius?: { val1: number; val2: number } };
  heap: Availability & {
    free_bytes?: number;
    allocated_bytes?: number;
    max_allocated_bytes?: number;
    total_bytes?: number;
  };
  memory?: Availability & {
    coverage?: string;
    pressure_pct_x100?: number;
    limiting_component?: string;
    limiting_name?: string;
    current_pressure?: MemoryPressureSnapshot;
    peak_pressure?: MemoryPressureSnapshot;
    system_heap_pressure_pct_x100?: number;
    physical?: {
      total_bytes?: number;
      image_reserved_bytes?: number;
      reserved_pct_x100?: number;
    };
    stacks?: {
      thread_count?: number;
      measured_count?: number;
      error_count?: number;
      total_bytes?: number;
      used_high_water_bytes?: number;
      max_pressure_pct_x100?: number;
      max_pressure_thread?: string;
    };
  };
  runtime: Availability & { uptime_ms?: number; uptime_seconds?: number };
  cpu: Availability & {
    active_pct_x100?: number;
    window_ms?: number;
    busy_cycles_delta?: number;
    total_cycles_delta?: number;
  };
}

export interface BoardSnapshot {
  mcu?: string;
  usb?: string;
  powerOutputs: PowerOutput[];
  switches: SwitchState;
  gpios: SafeGpio[];
  watchdog: WatchdogStatus;
  monitoring: BoardMonitoring;
  adc: AdcReading[];
}

export type LogicAnalyzerTriggerType = "none" | "rising" | "falling" | "either";

export interface LogicAnalyzerConfig {
  selectedPins: number[];
  sampleRateHz: number;
  preSamples: number;
  postSamples: number;
  triggerType: LogicAnalyzerTriggerType;
  triggerPin: number;
}

export interface LogicAnalyzerCaptureConfig {
  pinCount: number;
  pinBase: number;
  sampleRateHz?: number;
  requestedSampleRateHz?: number;
  actualSampleRateHz?: number;
  samplePeriodPs?: number;
  backend?: string;
  selectedPins?: number[];
  triggerType?: LogicAnalyzerTriggerType;
  triggerPin?: number;
}

export type LogicAnalyzerState = "idle" | "armed" | "capturing" | "done" | "error";

export interface LogicAnalyzerSample {
  timestampUs: number;
  values: number;
}

export interface LogicAnalyzerCapture {
  state: LogicAnalyzerState;
  config: LogicAnalyzerCaptureConfig;
  sampleCount: number;
  triggerIndex: number;
  samples: LogicAnalyzerSample[];
}
