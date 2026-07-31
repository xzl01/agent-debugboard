import { useEffect, useMemo, useRef, useState } from "react";
import type { RefObject } from "react";
import { Activity, Download, Eye, EyeOff, Play, Square, TerminalSquare, TimerReset, Zap } from "lucide-react";
import { Badge, Button, Card, Toggle } from "./ui";
import type { CaptureConfig, PowerCapture, PowerOutput } from "@/lib/types";
import type { SerialAutomationHandle, SerialChannelId } from "./SerialCard";
import { nominalVoltage, powerRailLabel, USER_POWER_RAILS } from "@/lib/power";
import { useI18n } from "@/lib/i18n";
import { buildPowerChartAxis, PowerChartYAxis } from "./PowerChartAxis";
import { summarizePowerCapture } from "@/lib/powerCapture";
import { exportPowerCaptureToFile } from "@/lib/powerCaptureExport";
import { appendStartupRunHistory } from "@/lib/startupRunHistory";
import {
  detectStartupMilestones,
  type BootloaderMode,
  type DetectedBootloader,
} from "@/lib/startupMilestones";
import {
  commandEnvelope,
  commandMarker,
  nextSerialLoginAction,
  parseCommandCompletion,
  stripTerminalControl,
  type SerialLoginState,
} from "@/lib/serialTask";
import type { AutomationTaskControl } from "@/lib/automationTask";

type Phase = "idle" | "powering_off" | "waiting" | "arming" | "capturing" | "retrying" | "logging_in" | "executing" | "finalizing" | "complete" | "partial" | "error" | "cancelled";
type MilestoneKey = "bootrom" | "firmware" | "kernel" | "login";

interface StartupAutomation {
  enabled: boolean;
  state: SerialLoginState | "disabled";
  username: string;
  command: string;
  marker: string;
  commandRx: string;
  commandOutput: string;
  exitCode: number | null;
  error: string | null;
  commandStartedAtMs: number | null;
  commandFinishedAtMs: number | null;
}

interface StartupRun {
  id: number;
  rail: string;
  serialChannel: SerialChannelId;
  rateHz: number;
  attempt: number;
  startedAt: number;
  poweredOnAtMs: number | null;
  finishedAt: number | null;
  milestones: Partial<Record<MilestoneKey, number>>;
  serial: string[];
  serialBuffer: string;
  bootloaderMode: BootloaderMode;
  detectedBootloader?: DetectedBootloader;
  prePowerBytes: number;
  postPowerBytes: number;
  postPowerMeaningfulBytes: number;
  lastPrePowerRxAtMs: number | null;
  serialComplete: boolean;
  timedOut: boolean;
  completionStarted: boolean;
  captureStopRequested: boolean;
  initialCaptureIds: Set<number>;
  automation: StartupAutomation;
  capture?: PowerCapture;
}

interface RunStats {
  durationMs: number;
  peakCurrentA: number;
  averagePowerW: number;
  energyJ: number;
}

type StartupStageKey = "power_on" | "boot" | "u_boot" | "uefi" | "kernel" | "userspace";

interface StartupStage {
  key: StartupStageKey;
  label: string;
  startedAtMs: number;
}

const MILESTONE_COLORS: Record<MilestoneKey, string> = {
  bootrom: "#64748b",
  firmware: "#f59e0b",
  kernel: "#4f7cff",
  login: "#22c55e",
};

const STAGE_COLORS: Record<StartupStageKey, string> = {
  power_on: "#94a3b8",
  boot: "#06b6d4",
  u_boot: "#f59e0b",
  uefi: "#f59e0b",
  kernel: "#4f7cff",
  userspace: "#22c55e",
};

function sleep(ms: number) {
  return new Promise<void>((resolve) => window.setTimeout(resolve, ms));
}

const DISCHARGED_CURRENT_UA = 20_000;
const POWER_STATE_SETTLE_TIMEOUT_MS = 5_000;
// A healthy target on the tested boards emits its first UART byte well below
// one second. Five seconds leaves ample boot-ROM/firmware margin while avoiding
// a long wait in the reproducible low-current state that needs a second cycle.
const FIRST_UART_RETRY_TIMEOUT_MS = 5_000;
const AUTOMATION_STORAGE_KEY = "linkr-startup-automation-profile-v1";

interface SavedAutomationProfile {
  username: string;
  password: string;
  command: string;
}

function loadAutomationProfile(): SavedAutomationProfile {
  const fallback = { username: "radxa", password: "radxa", command: "uname -a" };
  if (typeof localStorage === "undefined") return fallback;
  try {
    const saved = JSON.parse(localStorage.getItem(AUTOMATION_STORAGE_KEY) ?? "null");
    if (!saved || typeof saved !== "object") return fallback;
    return {
      username: typeof saved.username === "string" ? saved.username : fallback.username,
      password: typeof saved.password === "string" ? saved.password : fallback.password,
      command: typeof saved.command === "string" ? saved.command : fallback.command,
    };
  } catch {
    return fallback;
  }
}

function readingPower(capture: PowerCapture, rail: string) {
  const voltage = nominalVoltage(rail) ?? 0;
  const triggerTime = capture.samples[capture.triggerOffset]?.deviceTimeUs ?? 0;
  return capture.samples.flatMap((sample) => {
    const reading = sample.readings.find((item) => item.name === rail);
    if (!reading) return [];
    const currentA = reading.power_enabled ? Math.max(0, reading.current_ua / 1_000_000) : 0;
    return [{
      xMs: (sample.deviceTimeUs - triggerTime) / 1000,
      currentA,
      powerW: currentA * voltage,
    }];
  });
}

function startupStages(run: StartupRun): StartupStage[] {
  const bootloader = run.detectedBootloader === "uefi" || run.bootloaderMode === "uefi"
    ? { key: "uefi" as const, label: "UEFI" }
    : { key: "u_boot" as const, label: "U-Boot" };
  const stages: StartupStage[] = [
    { key: "power_on", label: "Power-on", startedAtMs: 0 },
  ];
  if (run.milestones.bootrom != null) {
    stages.push({ key: "boot", label: "Boot", startedAtMs: run.milestones.bootrom });
  }
  if (run.milestones.firmware != null) {
    stages.push({ ...bootloader, startedAtMs: run.milestones.firmware });
  }
  if (run.milestones.kernel != null) {
    stages.push({ key: "kernel", label: "Kernel", startedAtMs: run.milestones.kernel });
  }
  if (run.milestones.login != null) {
    stages.push({ key: "userspace", label: "Login / userspace", startedAtMs: run.milestones.login });
  }
  return stages;
}

function startupStageAt(run: StartupRun, relativeMs: number): StartupStage {
  const stages = startupStages(run);
  return [...stages].reverse().find((stage) => relativeMs >= stage.startedAtMs) ?? stages[0];
}

function calculateStats(run: StartupRun): RunStats | null {
  if (!run.capture) return null;
  const summary = summarizePowerCapture(run.capture, run.rail);
  if (summary.durationMs <= 0) return null;
  return {
    durationMs: summary.durationMs,
    peakCurrentA: summary.peakCurrentA,
    averagePowerW: summary.averagePowerW,
    energyJ: summary.wattHours * 3600,
  };
}

function downloadBlob(filename: string, content: string, type: string) {
  const url = URL.createObjectURL(new Blob([content], { type }));
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = filename;
  anchor.hidden = true;
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function downloadSerial(run: StartupRun) {
  // Preserve the target's byte-to-text stream exactly. Prefixing every USB
  // chunk with a timestamp can split and corrupt otherwise valid log lines.
  downloadBlob(`linkr-startup-${run.id}-${run.serialChannel}-serial.log`, run.serial.join(""), "text/plain;charset=utf-8");
}

async function downloadPowerData(run: StartupRun, format: "csv" | "ndjson") {
  const capture = run.capture;
  if (!capture) return;
  const triggerTimeUs = capture.triggerDeviceTimeUs ??
    capture.samples.find((sample) => sample.triggered)?.deviceTimeUs ?? 0;
  const extraColumns = [
    "run_id",
    "attempt",
    "serial_channel",
    "detected_bootloader",
    "relative_ms",
    "stage",
    "stage_label",
    "stage_elapsed_ms",
  ] as const;
  await exportPowerCaptureToFile(capture, format, undefined, {
    fileName: `linkr-startup-${run.id}-power.${format}`,
    extraColumns,
    extraValues: (sample) => {
      const relativeMs = (sample.deviceTimeUs - triggerTimeUs) / 1000;
      const stage = startupStageAt(run, relativeMs);
      return {
        run_id: run.id,
        attempt: run.attempt,
        serial_channel: run.serialChannel,
        detected_bootloader: run.detectedBootloader ?? run.bootloaderMode,
        relative_ms: relativeMs,
        stage: stage.key,
        stage_label: stage.label,
        stage_elapsed_ms: Math.max(0, relativeMs - stage.startedAtMs),
      };
    },
  });
}

function taskPassed(run: StartupRun) {
  return !!run.capture && !run.capture.incomplete && (run.capture.droppedSamples ?? 0) === 0 &&
    run.milestones.login != null &&
    (!run.automation.enabled || run.automation.state === "passed");
}

function taskReport(run: StartupRun) {
  const stats = calculateStats(run);
  return {
    schema: "radxa-linkr-task-result.v1",
    run_id: run.id,
    status: taskPassed(run) ? "passed" : "failed",
    rail: run.rail,
    serial_channel: run.serialChannel,
    attempt: run.attempt,
    started_at: new Date(run.startedAt).toISOString(),
    finished_at: run.finishedAt == null ? null : new Date(run.finishedAt).toISOString(),
    timed_out: run.timedOut,
    bootloader: run.detectedBootloader ?? run.bootloaderMode,
    serial: {
      raw_rx_bytes: run.postPowerBytes,
      meaningful_rx_bytes: run.postPowerMeaningfulBytes,
      ignored_control_bytes: Math.max(0, run.postPowerBytes - run.postPowerMeaningfulBytes),
    },
    milestones_ms: run.milestones,
    power: stats ? {
      duration_ms: stats.durationMs,
      peak_current_a: stats.peakCurrentA,
      average_power_w: stats.averagePowerW,
      energy_j: stats.energyJ,
      sample_count: run.capture?.sampleCount ?? run.capture?.samples.length ?? 0,
      dropped_samples: run.capture?.droppedSamples ?? 0,
      incomplete: run.capture?.incomplete ?? false,
      interruption_reason: run.capture?.interruptionReason ?? null,
      archive_id: run.capture?.archiveId ?? null,
    } : null,
    login: run.automation.enabled ? {
      username: run.automation.username,
      state: run.automation.state,
      error: run.automation.error,
    } : null,
    command: run.automation.enabled ? {
      text: run.automation.command,
      exit_code: run.automation.exitCode,
      duration_ms: run.automation.commandStartedAtMs != null && run.automation.commandFinishedAtMs != null
        ? run.automation.commandFinishedAtMs - run.automation.commandStartedAtMs
        : null,
      output: run.automation.commandOutput,
    } : null,
  };
}

function downloadTaskReport(run: StartupRun) {
  downloadBlob(
    `linkr-startup-${run.id}-result.json`,
    JSON.stringify(taskReport(run), null, 2) + "\n",
    "application/json;charset=utf-8"
  );
}

export function StartupPowerAnalysis({
  outputs,
  captureState,
  captures,
  serialRef,
  onSetPower,
  onReadPower,
  onArmCapture,
  onStopCapture,
  onCancelCapture,
  taskControl,
}: {
  outputs: PowerOutput[];
  captureState: "idle" | "connecting" | "armed" | "recording" | "receiving";
  captures: PowerCapture[];
  serialRef: RefObject<SerialAutomationHandle>;
  onSetPower: (name: string, on: boolean) => Promise<void>;
  onReadPower: (name: string) => Promise<{ state: string; currentUa: number }>;
  onArmCapture: (config: CaptureConfig) => Promise<void>;
  onStopCapture: () => void;
  onCancelCapture: () => void;
  taskControl: AutomationTaskControl;
}) {
  const { t } = useI18n();
  const [phase, setPhase] = useState<Phase>("idle");
  const [rail, setRail] = useState("5v_out");
  const [serialChannel, setSerialChannel] = useState<SerialChannelId>("uart0");
  const [offDelayMs, setOffDelayMs] = useState(3000);
  const [rateHz, setRateHz] = useState(50);
  const [timeoutSeconds, setTimeoutSeconds] = useState(90);
  const [bootloaderMode, setBootloaderMode] = useState<BootloaderMode>("auto");
  const [automationEnabled, setAutomationEnabled] = useState(false);
  const [savedAutomation] = useState(loadAutomationProfile);
  const [username, setUsername] = useState(savedAutomation.username);
  const [password, setPassword] = useState(savedAutomation.password);
  const [command, setCommand] = useState(savedAutomation.command);
  const [rememberCredentials, setRememberCredentials] = useState(() =>
    typeof localStorage !== "undefined" && localStorage.getItem(AUTOMATION_STORAGE_KEY) != null
  );
  const [showPassword, setShowPassword] = useState(false);
  const [activeRun, setActiveRun] = useState<StartupRun | null>(null);
  const [runs, setRuns] = useState<StartupRun[]>([]);
  const [error, setError] = useState<string | null>(null);
  const activeRunRef = useRef<StartupRun | null>(null);
  const unsubscribeRef = useRef<(() => void) | null>(null);
  const timeoutRef = useRef<number | null>(null);
  const publishTimerRef = useRef<number | null>(null);
  const operationRef = useRef(0);
  const onSetPowerRef = useRef(onSetPower);
  const automationPasswordRef = useRef("");

  useEffect(() => {
    if (rememberCredentials) {
      localStorage.setItem(AUTOMATION_STORAGE_KEY, JSON.stringify({ username, password, command }));
    } else {
      localStorage.removeItem(AUTOMATION_STORAGE_KEY);
    }
  }, [command, password, rememberCredentials, username]);

  useEffect(() => {
    onSetPowerRef.current = onSetPower;
  }, [onSetPower]);

  const publishRun = (run: StartupRun) => {
    activeRunRef.current = run;
    setActiveRun({
      ...run,
      milestones: { ...run.milestones },
      // The complete log remains in activeRunRef. Copying an ever-growing log
      // into React state for every UART chunk can stall the read loop.
      serial: [],
      initialCaptureIds: new Set(run.initialCaptureIds),
    });
  };

  const schedulePublishRun = (run: StartupRun) => {
    activeRunRef.current = run;
    if (publishTimerRef.current != null) return;
    publishTimerRef.current = window.setTimeout(() => {
      publishTimerRef.current = null;
      if (activeRunRef.current === run) publishRun(run);
    }, 100);
  };

  const clearRuntime = () => {
    unsubscribeRef.current?.();
    unsubscribeRef.current = null;
    if (timeoutRef.current != null) window.clearTimeout(timeoutRef.current);
    timeoutRef.current = null;
    if (publishTimerRef.current != null) window.clearTimeout(publishTimerRef.current);
    publishTimerRef.current = null;
  };

  const completeRun = async (run: StartupRun, timedOut = false) => {
    if (run.completionStarted) return;
    run.completionStarted = true;
    clearRuntime();
    setPhase("finalizing");
    publishRun(run);
    try {
      // Completion is not safe until the target rail is confirmed on. This is
      // deliberately idempotent and also heals an interrupted power-on call.
      await onSetPowerRef.current(run.rail, true);
    } catch (reason) {
      serialRef.current?.setAutomationActive(false, run.serialChannel);
      automationPasswordRef.current = "";
      taskControl.release("startup");
      run.finishedAt = Date.now();
      activeRunRef.current = null;
      setActiveRun(null);
      setPhase("error");
      setError(t("startup.error.restorePower").replaceAll("{rail}", powerRailLabel(run.rail)) +
        ` ${reason instanceof Error ? reason.message : String(reason)}`);
      return;
    }
    run.finishedAt = Date.now();
    run.timedOut = timedOut;
    serialRef.current?.setAutomationActive(false, run.serialChannel);
    automationPasswordRef.current = "";
    taskControl.release("startup");
    activeRunRef.current = null;
    setActiveRun(null);
    const completedRun = {
      ...run,
      serial: [...run.serial],
      milestones: { ...run.milestones },
    };
    setRuns((previous) => appendStartupRunHistory(previous, completedRun));
    if (!run.capture) {
      setPhase("error");
      setError(t("startup.error.captureMissing"));
    } else if (taskPassed(run)) {
      setPhase("complete");
      setError(null);
    } else {
      setPhase("partial");
      const serialConnected = serialRef.current?.isConnected(run.serialChannel) ?? false;
      setError(run.capture.incomplete || (run.capture.droppedSamples ?? 0) > 0
        ? t("startup.error.captureIncomplete")
          .replaceAll("{dropped}", String(run.capture.droppedSamples ?? 0))
        : run.automation.error ??
        t(!serialConnected ? "startup.error.disconnected" : run.postPowerMeaningfulBytes === 0 ? "startup.error.noSerial" : "startup.error.noLogin"));
    }
  };

  const maybeCompleteRun = (run: StartupRun) => {
    if (!run.capture && run.serialComplete && !run.captureStopRequested) {
      run.captureStopRequested = true;
      onStopCapture();
    }
    if (run.capture && run.serialComplete) void completeRun(run, run.timedOut);
  };

  const failRun = async (message: string, restorePower: boolean) => {
    operationRef.current += 1;
    clearRuntime();
    const run = activeRunRef.current;
    if (run) serialRef.current?.setAutomationActive(false, run.serialChannel);
    automationPasswordRef.current = "";
    taskControl.release("startup");
    activeRunRef.current = null;
    setActiveRun(null);
    setPhase("error");
    setError(message);
    if (captureState !== "idle") onCancelCapture();
    if (restorePower && run) {
      try { await onSetPower(run.rail, true); } catch { /* Preserve the primary workflow error. */ }
    }
  };

  useEffect(() => () => {
    operationRef.current += 1;
    const run = activeRunRef.current;
    clearRuntime();
    if (run) {
      serialRef.current?.setAutomationActive(false, run.serialChannel);
      void onSetPowerRef.current(run.rail, true);
    }
    automationPasswordRef.current = "";
    taskControl.release("startup");
  }, [taskControl.release]);

  useEffect(() => {
    const run = activeRunRef.current;
    if (!run || run.capture) return;
    const capture = [...captures].reverse().find((item) =>
      !run.initialCaptureIds.has(item.id) && item.trigger === "power_on" && item.source === run.rail
    );
    if (!capture) return;
    run.capture = capture;
    publishRun(run);
    maybeCompleteRun(run);
  }, [captures]);

  const start = async () => {
    const serial = serialRef.current;
    const output = outputs.find((item) => item.name === rail);
    if (!serial?.isConnected(serialChannel)) {
      setError(t("startup.error.serial").replaceAll("{channel}", serialChannel.toUpperCase()));
      return;
    }
    if (!output?.controllable) {
      setError(t("startup.error.power"));
      return;
    }
    if (automationEnabled && (!username.trim() || !command.trim())) {
      setError(t("startup.error.automationConfig"));
      return;
    }
    if (captureState !== "idle") {
      setError(t("startup.error.captureBusy"));
      return;
    }
    if (!window.confirm(t("startup.confirm").replaceAll("{rail}", powerRailLabel(rail)).replaceAll("{delay}", String(offDelayMs)))) return;
    if (!taskControl.acquire("startup")) {
      setError(t("startup.error.taskBusy"));
      return;
    }

    const operation = operationRef.current + 1;
    operationRef.current = operation;
    setError(null);
    automationPasswordRef.current = password;
    serial.setAutomationActive(automationEnabled, serialChannel);
    serial.clear(serialChannel);
    const runId = Date.now();
    const run: StartupRun = {
      id: runId,
      rail,
      serialChannel,
      rateHz,
      attempt: 1,
      startedAt: Date.now(),
      poweredOnAtMs: null,
      finishedAt: null,
      milestones: {},
      serial: [],
      serialBuffer: "",
      bootloaderMode,
      prePowerBytes: 0,
      postPowerBytes: 0,
      postPowerMeaningfulBytes: 0,
      lastPrePowerRxAtMs: null,
      serialComplete: false,
      timedOut: false,
      completionStarted: false,
      captureStopRequested: false,
      initialCaptureIds: new Set(captures.map((item) => item.id)),
      automation: {
        enabled: automationEnabled,
        state: automationEnabled ? "waiting" : "disabled",
        username: username.trim(),
        command: command.trim(),
        marker: commandMarker(runId),
        commandRx: "",
        commandOutput: "",
        exitCode: null,
        error: null,
        commandStartedAtMs: null,
        commandFinishedAtMs: null,
      },
    };
    publishRun(run);
    unsubscribeRef.current = serial.subscribe((text, receivedAtMs) => {
      const current = activeRunRef.current;
      if (!current) return;
      const byteLength = new TextEncoder().encode(text).byteLength;
      const meaningfulText = stripTerminalControl(text);
      const meaningfulByteLength = new TextEncoder().encode(meaningfulText).byteLength;
      if (current.poweredOnAtMs == null) {
        current.prePowerBytes += byteLength;
        current.lastPrePowerRxAtMs = receivedAtMs;
        return;
      }
      current.postPowerBytes += byteLength;
      if (meaningfulByteLength === 0) {
        schedulePublishRun(current);
        return;
      }
      current.serial.push(meaningfulText);
      current.postPowerMeaningfulBytes += meaningfulByteLength;
      current.serialBuffer = (current.serialBuffer + meaningfulText).slice(-16384);
      const detected = detectStartupMilestones(current.serialBuffer, current.bootloaderMode);
      const elapsedMs = Math.max(0, receivedAtMs - current.poweredOnAtMs);
      // Boot ROM banners vary widely and many platforms print none at all.
      // The first meaningful post-power UART text is the only portable observed boot mark.
      if (current.milestones.bootrom == null) {
        current.milestones.bootrom = elapsedMs;
      }
      if (current.milestones.firmware == null && detected.bootloader) {
        current.milestones.firmware = elapsedMs;
        current.detectedBootloader = detected.bootloader;
      }
      if (current.milestones.kernel == null && detected.kernel) {
        current.milestones.kernel = elapsedMs;
      }
      if (!current.automation.enabled && current.milestones.login == null && detected.login) {
        current.milestones.login = elapsedMs;
        current.serialComplete = true;
      }

      if (current.automation.enabled) {
        const automation = current.automation;
        if (automation.state === "command_running") {
          automation.commandRx = (automation.commandRx + meaningfulText).slice(-65_536);
          const completion = parseCommandCompletion(automation.commandRx, automation.marker);
          if (completion) {
            automation.exitCode = completion.exitCode;
            automation.commandOutput = completion.output.slice(-16_384);
            automation.commandFinishedAtMs = elapsedMs;
            automation.state = completion.exitCode === 0 ? "passed" : "failed";
            automation.error = completion.exitCode === 0
              ? null
              : t("startup.error.commandFailed").replaceAll("{code}", String(completion.exitCode));
            current.serialComplete = true;
          }
        } else if (["waiting", "username_sent", "password_sent"].includes(automation.state)) {
          const action = nextSerialLoginAction(current.serialBuffer, automation.state as SerialLoginState);
          let write: Promise<void> | null = null;
          if (action === "send_username") {
            automation.state = "username_sent";
            setPhase("logging_in");
            write = serial.write(`${automation.username}\r\n`, current.serialChannel);
          } else if (action === "send_password") {
            automation.state = "password_sent";
            setPhase("logging_in");
            write = serial.write(`${automationPasswordRef.current}\r\n`, current.serialChannel);
          } else if (action === "run_command") {
            if (current.milestones.login == null) current.milestones.login = elapsedMs;
            automation.state = "command_running";
            automation.commandStartedAtMs = elapsedMs;
            automation.commandRx = "";
            setPhase("executing");
            write = serial.write(
              commandEnvelope(automation.command, automation.marker),
              current.serialChannel
            );
          }
          if (write) {
            void write.catch((reason) => {
              if (activeRunRef.current !== current) return;
              automation.state = "failed";
              automation.error = t("startup.error.serialWrite") +
                ` ${reason instanceof Error ? reason.message : String(reason)}`;
              current.serialComplete = true;
              schedulePublishRun(current);
              maybeCompleteRun(current);
            });
          }
        }
      }
      schedulePublishRun(current);
      maybeCompleteRun(current);
    }, serialChannel);

    const readPowerUntil = async (
      predicate: (reading: { state: string; currentUa: number }) => boolean,
      timeoutMs: number
    ) => {
      const deadline = performance.now() + timeoutMs;
      let lastReading: { state: string; currentUa: number } | null = null;
      let lastError: unknown = null;
      do {
        try {
          lastReading = await onReadPower(rail);
          lastError = null;
          if (predicate(lastReading)) return lastReading;
        } catch (reason) {
          // A power transition can briefly coincide with an exhausted HTTP
          // client slot. Preserve the last error, but keep polling within the
          // existing bounded settle window before declaring the task failed.
          lastError = reason;
        }
        await sleep(100);
        if (operationRef.current !== operation) return null;
      } while (performance.now() < deadline);
      if (lastReading) return lastReading;
      throw lastError instanceof Error ? lastError : new Error(String(lastError ?? "power read failed"));
    };

    const powerOffAndWait = async () => {
      setPhase("powering_off");
      await onSetPower(rail, false);
      if (operationRef.current !== operation) return;
      setPhase("waiting");
      await sleep(offDelayMs);
      if (operationRef.current !== operation) return;
      const quietDeadline = performance.now() + 2000;
      while (run.lastPrePowerRxAtMs != null && performance.now() - run.lastPrePowerRxAtMs < 250 && performance.now() < quietDeadline) {
        await sleep(50);
        if (operationRef.current !== operation) return;
      }

      const discharged = await readPowerUntil(
        (reading) => reading.state === "off" && reading.currentUa <= DISCHARGED_CURRENT_UA,
        POWER_STATE_SETTLE_TIMEOUT_MS
      );
      if (!discharged) return;
      if (discharged.state !== "off" || discharged.currentUa > DISCHARGED_CURRENT_UA) {
        throw new Error(t("startup.error.notDischarged")
          .replaceAll("{rail}", powerRailLabel(rail))
          .replaceAll("{current}", `${(discharged.currentUa / 1000).toFixed(1)} mA`));
      }
    };

    const armAndPowerOn = async () => {
      // Arm only after the rail has been switched off and the configured
      // discharge wait has elapsed. This makes the physical sequence match a
      // manual off -> wait -> on cycle and keeps high-rate capture traffic out
      // of the shutdown edge.
      setPhase("arming");
      await onArmCapture({
        trigger: "power_on",
        source: rail,
        edge: "rising",
        thresholdUa: 0,
        rateHz,
        preSamples: 0,
        postSamples: Math.max(1, timeoutSeconds * rateHz),
        streaming: true,
        stopAfterMs: timeoutSeconds * 1000,
      });
      if (operationRef.current !== operation) return;

      // Keep the capture acknowledgement and the physical power-on command in
      // the same awaited transaction. A React state/effect transition is not a
      // reliable control edge for hardware.
      run.serial = [];
      run.serialBuffer = "";
      run.postPowerBytes = 0;
      run.postPowerMeaningfulBytes = 0;
      run.poweredOnAtMs = performance.now();
      publishRun(run);
      setPhase("capturing");
      await onSetPower(rail, true);
      if (operationRef.current !== operation) return;
      const powered = await readPowerUntil(
        (reading) => reading.state === "on",
        POWER_STATE_SETTLE_TIMEOUT_MS
      );
      if (!powered) return;
      if (powered.state !== "on") {
        throw new Error(t("startup.error.notPowered").replaceAll("{rail}", powerRailLabel(rail)));
      }
    };

    const runMeasuredPowerCycle = async () => {
      await powerOffAndWait();
      if (operationRef.current !== operation) return;
      await armAndPowerOn();
    };

    try {
      await runMeasuredPowerCycle();

      // Some targets can reproducibly enter a low-current pre-UART state after
      // the first cold cycle. Match the proven manual recovery: if no byte is
      // observed within a conservative first-UART window, discard the first
      // capture and perform one measured retry automatically instead of
      // waiting for the full serial timeout.
      const firstUartDeadline = performance.now() + FIRST_UART_RETRY_TIMEOUT_MS;
      while (run.postPowerMeaningfulBytes === 0 && performance.now() < firstUartDeadline) {
        await sleep(100);
        if (operationRef.current !== operation) return;
      }
      if (run.postPowerMeaningfulBytes === 0) {
        setPhase("retrying");
        onCancelCapture();
        await sleep(100);
        if (operationRef.current !== operation) return;
        run.attempt = 2;
        run.capture = undefined;
        run.captureStopRequested = false;
        run.milestones = {};
        run.detectedBootloader = undefined;
        run.poweredOnAtMs = null;
        run.automation.state = run.automation.enabled ? "waiting" : "disabled";
        run.automation.commandRx = "";
        run.automation.commandOutput = "";
        run.automation.exitCode = null;
        run.automation.error = null;
        run.automation.commandStartedAtMs = null;
        run.automation.commandFinishedAtMs = null;
        serial.clear(run.serialChannel);
        publishRun(run);
        await runMeasuredPowerCycle();
      }

      const captureStartedAt = run.poweredOnAtMs ?? performance.now();
      const remainingTimeoutMs = Math.max(1_000,
        (timeoutSeconds * 1000) - (performance.now() - captureStartedAt));
      timeoutRef.current = window.setTimeout(() => {
        const current = activeRunRef.current;
        if (!current) return;
        if (current.automation.enabled && current.automation.state !== "passed" && current.automation.state !== "failed") {
          current.automation.state = "failed";
          current.automation.error = t("startup.error.automationTimeout");
        }
        current.serialComplete = true;
        current.timedOut = true;
        publishRun(current);
        maybeCompleteRun(current);
      }, remainingTimeoutMs);
    } catch (reason) {
      await failRun(reason instanceof Error ? reason.message : String(reason), true);
    }
  };

  const cancel = async () => {
    const run = activeRunRef.current;
    operationRef.current += 1;
    clearRuntime();
    if (captureState !== "idle") onCancelCapture();
    activeRunRef.current = null;
    setActiveRun(null);
    setPhase("cancelled");
    taskControl.release("startup");
    if (run) {
      serialRef.current?.setAutomationActive(false, run.serialChannel);
      automationPasswordRef.current = "";
      try { await onSetPower(run.rail, true); } catch (reason) {
        setError(reason instanceof Error ? reason.message : String(reason));
        setPhase("error");
      }
    }
  };

  const stopAndReport = () => {
    const run = activeRunRef.current;
    if (!run) return;
    if (run.automation.enabled && run.automation.state !== "passed" && run.automation.state !== "failed") {
      run.automation.state = "failed";
      run.automation.error = t("startup.error.stoppedBeforeCommand");
    }
    run.serialComplete = true;
    publishRun(run);
    maybeCompleteRun(run);
  };

  const current = runs.at(-1);
  const previous = current ? [...runs.slice(0, -1)].reverse().find((run) => run.rail === current.rail) : undefined;
  const chartRuns = current ? [previous, current].filter((run): run is StartupRun => !!run?.capture) : [];
  const chartSeries = useMemo(() => chartRuns.map((run) => ({ run, points: readingPower(run.capture!, run.rail) })), [runs]);
  const allPoints = chartSeries.flatMap((series) => series.points);
  const minX = Math.min(-1, ...allPoints.map((point) => point.xMs));
  const maxX = Math.max(1, ...allPoints.map((point) => point.xMs));
  const chartAxis = buildPowerChartAxis(
    "power",
    Math.max(0.01, ...allPoints.map((point) => point.powerW)),
  );
  const maxY = chartAxis.maximum;
  const currentStats = current ? calculateStats(current) : null;
  const previousStats = previous ? calculateStats(previous) : null;
  const busy = ["powering_off", "waiting", "arming", "capturing", "retrying", "logging_in", "executing", "finalizing"].includes(phase);
  const blockedByOtherTask = taskControl.owner != null && taskControl.owner !== "startup";
  const displayRun = activeRun ?? current;
  const firmwareLabel = displayRun?.detectedBootloader === "uefi" || displayRun?.bootloaderMode === "uefi"
    ? "UEFI"
    : displayRun?.detectedBootloader === "uboot" || displayRun?.bootloaderMode === "uboot"
      ? "U-Boot"
      : "U-Boot / UEFI";
  const currentStages = current ? startupStages(current) : [];
  const currentStageBands = currentStages.flatMap((stage, index) => {
    const nextStart = currentStages[index + 1]?.startedAtMs ?? maxX;
    const start = Math.max(minX, stage.startedAtMs);
    const end = Math.min(maxX, nextStart);
    return end > start ? [{ ...stage, start, end }] : [];
  });
  const currentMilestoneMarkers = current ? ([
    { key: "bootrom" as const, label: "Boot", xMs: current.milestones.bootrom },
    { key: "firmware" as const, label: current.detectedBootloader === "uefi" || current.bootloaderMode === "uefi" ? "UEFI" : "U-Boot", xMs: current.milestones.firmware },
    { key: "kernel" as const, label: "Kernel", xMs: current.milestones.kernel },
    { key: "login" as const, label: "Login", xMs: current.milestones.login },
  ]).flatMap((marker) => marker.xMs != null ? [{ ...marker, xMs: marker.xMs }] : []) : [];

  return (
    <Card title={t("startup.title")} subtitle={t("startup.subtitle")} icon={TimerReset}>
      <div className="grid gap-2 sm:grid-cols-2">
        <label className="text-[11px] text-ink-dim">{t("startup.rail")}
          <select value={rail} onChange={(event) => setRail(event.target.value)} disabled={busy}
            className="mt-1 w-full rounded-lg border border-line bg-panel2 px-2 py-2 text-xs text-ink">
            {USER_POWER_RAILS.filter((name) => outputs.some((item) => item.name === name && item.controllable)).map((name) =>
              <option key={name} value={name}>{powerRailLabel(name)}</option>
            )}
          </select>
        </label>
        <label className="text-[11px] text-ink-dim">{t("startup.offDelay")}
          <input type="number" min="250" max="10000" step="250" value={offDelayMs} onChange={(event) => setOffDelayMs(Number(event.target.value))} disabled={busy}
            className="mt-1 w-full rounded-lg border border-line bg-panel2 px-2 py-2 text-xs text-ink" />
        </label>
        <label className="text-[11px] text-ink-dim">{t("startup.rate")}
          <select value={rateHz} onChange={(event) => setRateHz(Number(event.target.value))} disabled={busy}
            className="mt-1 w-full rounded-lg border border-line bg-panel2 px-2 py-2 text-xs text-ink">
            {[50, 100, 250, 500, 1000].map((rate) => <option key={rate} value={rate}>{rate} Hz</option>)}
          </select>
        </label>
        <label className="text-[11px] text-ink-dim">{t("startup.serialChannel")}
          <select value={serialChannel} onChange={(event) => setSerialChannel(event.target.value as SerialChannelId)} disabled={busy}
            className="mt-1 w-full rounded-lg border border-line bg-panel2 px-2 py-2 text-xs text-ink">
            <option value="uart0">UART0</option>
            <option value="uart1">UART1</option>
          </select>
        </label>
        <label className="text-[11px] text-ink-dim">{t("startup.bootloader")}
          <select value={bootloaderMode} onChange={(event) => setBootloaderMode(event.target.value as BootloaderMode)} disabled={busy}
            className="mt-1 w-full rounded-lg border border-line bg-panel2 px-2 py-2 text-xs text-ink">
            <option value="auto">{t("startup.bootloader.auto")}</option>
            <option value="uboot">U-Boot</option>
            <option value="uefi">{t("startup.bootloader.uefi")}</option>
          </select>
        </label>
        <label className="text-[11px] text-ink-dim">{t("startup.timeout")}
          <input type="number" min="5" max="300" value={timeoutSeconds} onChange={(event) => setTimeoutSeconds(Number(event.target.value))} disabled={busy}
            className="mt-1 w-full rounded-lg border border-line bg-panel2 px-2 py-2 text-xs text-ink" />
        </label>
        <div className="text-[11px] text-ink-dim">{t("startup.window")}
          <div className="mt-1 rounded-lg border border-line bg-panel2/55 px-2 py-2 font-mono text-xs text-ink">
            {timeoutSeconds.toFixed(1)} s
          </div>
        </div>
      </div>

      <div className="mt-3 overflow-hidden rounded-xl border border-line/70 bg-panel2/30">
        <div className="flex items-center gap-3 px-3 py-2.5">
          <span className="grid h-8 w-8 shrink-0 place-items-center rounded-lg bg-brand/10 text-brand">
            <TerminalSquare size={15} />
          </span>
          <div className="min-w-0 flex-1">
            <div className="text-xs font-semibold text-ink">{t("startup.automation.title")}</div>
            <div className="truncate text-[10px] text-ink-dim">{t("startup.automation.subtitle")}</div>
          </div>
          <Toggle checked={automationEnabled} onChange={setAutomationEnabled} disabled={busy} />
        </div>
        {automationEnabled && <div className="grid gap-2 border-t border-line/60 p-3 sm:grid-cols-2">
          <label className="text-[11px] text-ink-dim">{t("startup.automation.username")}
            <input value={username} onChange={(event) => setUsername(event.target.value)} disabled={busy}
              autoComplete="username"
              className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink outline-none focus-visible:ring-2 focus-visible:ring-brand/30" />
          </label>
          <label className="text-[11px] text-ink-dim">{t("startup.automation.password")}
            <span className="relative mt-1 block">
              <input type={showPassword ? "text" : "password"} value={password} onChange={(event) => setPassword(event.target.value)} disabled={busy}
                autoComplete="current-password"
                className="w-full rounded-lg border border-line bg-panel px-2 py-2 pr-9 text-xs text-ink outline-none focus-visible:ring-2 focus-visible:ring-brand/30" />
              <button type="button" onClick={() => setShowPassword((shown) => !shown)} disabled={busy}
                aria-label={showPassword ? t("startup.automation.hidePassword") : t("startup.automation.showPassword")}
                className="absolute inset-y-0 right-0 grid w-9 place-items-center text-ink-dim hover:text-ink disabled:opacity-40">
                {showPassword ? <EyeOff size={14} /> : <Eye size={14} />}
              </button>
            </span>
          </label>
          <label className="text-[11px] text-ink-dim sm:col-span-2">{t("startup.automation.command")}
            <textarea value={command} onChange={(event) => setCommand(event.target.value)} disabled={busy} rows={3}
              spellCheck={false}
              className="mt-1 w-full resize-y rounded-lg border border-line bg-panel px-2 py-2 font-mono text-xs text-ink outline-none focus-visible:ring-2 focus-visible:ring-brand/30" />
          </label>
          <label className="flex min-h-8 items-center gap-2 text-[11px] text-ink-dim sm:col-span-2">
            <input type="checkbox" checked={rememberCredentials} onChange={(event) => setRememberCredentials(event.target.checked)} disabled={busy}
              className="h-4 w-4 rounded border-line accent-[rgb(var(--c-brand))]" />
            {t("startup.automation.remember")}
          </label>
        </div>}
      </div>

      <div className="mt-3 flex flex-wrap items-center gap-2">
        {!busy ? (
          <Button variant="primary" disabled={blockedByOtherTask} onClick={() => void start()}><Play size={15} />{t("startup.start")}</Button>
        ) : (
          <Button onClick={() => void cancel()}><Square size={15} />{t("startup.cancel")}</Button>
        )}
        {phase === "capturing" && <Button variant="ghost" onClick={stopAndReport}>{t("startup.stop")}</Button>}
        <Badge tone={phase === "error" ? "danger" : phase === "partial" ? "warn" : busy ? "brand" : phase === "complete" ? "ok" : "neutral"}>
          {t(`startup.phase.${phase}`)}
        </Badge>
        {displayRun && <span className="text-[11px] text-ink-dim">{t("startup.rxAfterPower")} {displayRun.postPowerMeaningfulBytes.toLocaleString()} B</span>}
        {displayRun && displayRun.postPowerBytes > displayRun.postPowerMeaningfulBytes &&
          <span className="text-[11px] text-ink-dim">{t("startup.rxIgnored")} {(displayRun.postPowerBytes - displayRun.postPowerMeaningfulBytes).toLocaleString()} B</span>}
        {displayRun && displayRun.attempt > 1 && <span className="text-[11px] text-warn">{t("startup.retryUsed")}</span>}
        {displayRun && displayRun.prePowerBytes > 0 && <span className="text-[11px] text-ink-dim">{t("startup.rxDiscarded")} {displayRun.prePowerBytes.toLocaleString()} B</span>}
      </div>

      {error && <div className={`mt-3 rounded-lg px-3 py-2 text-xs ${phase === "partial" ? "border border-warn/30 bg-warn/10 text-warn" : "border border-danger/30 bg-danger/10 text-danger"}`}>{error}</div>}

      {(activeRun || current) && <div className="mt-4 border-t border-line/60 pt-3">
        <div className="grid grid-cols-2 gap-2 sm:grid-cols-4">
          {([
            { key: "bootrom" as const, label: t("startup.milestone.boot") },
            { key: "firmware" as const, label: firmwareLabel },
            { key: "kernel" as const, label: "Kernel" },
            { key: "login" as const, label: "Login" },
          ]).map((milestone) => {
            const run = activeRun ?? current!;
            const value = run.milestones[milestone.key];
            return <div key={milestone.key} className="rounded-lg border border-line/60 bg-panel2/40 px-2.5 py-2">
              <div className="text-[10px] text-ink-dim">{milestone.label}</div>
              <div className="mt-0.5 font-mono text-sm font-semibold text-ink">{value == null ? "—" : `${(value / 1000).toFixed(2)} s`}</div>
            </div>;
          })}
        </div>
      </div>}

      {current && currentStats && <div className="mt-3 grid grid-cols-2 gap-2 sm:grid-cols-4">
        <div><div className="text-[10px] text-ink-dim">{t("startup.peak")}</div><div className="font-mono text-sm font-semibold text-brand">{currentStats.peakCurrentA.toFixed(3)} A</div></div>
        <div><div className="text-[10px] text-ink-dim">{t("startup.average")}</div><div className="font-mono text-sm font-semibold text-ink">{currentStats.averagePowerW.toFixed(3)} W</div></div>
        <div><div className="text-[10px] text-ink-dim">{t("startup.energy")}</div><div className="font-mono text-sm font-semibold text-warn">{currentStats.energyJ.toFixed(3)} J</div><div className="text-[9px] text-ink-dim">{(currentStats.durationMs / 1000).toFixed(1)} s</div></div>
        <div><div className="text-[10px] text-ink-dim">{t("startup.compare")}</div><div className="font-mono text-xs font-semibold text-ink">
          {previousStats ? `${((currentStats.peakCurrentA - previousStats.peakCurrentA) * 1000).toFixed(0)} mA` : "—"}
        </div></div>
      </div>}

      {displayRun?.automation.enabled && <div className="mt-3 rounded-xl border border-line/70 bg-panel2/35 p-3">
        <div className="flex flex-wrap items-center gap-2">
          <TerminalSquare size={14} className="text-brand" />
          <span className="text-xs font-semibold text-ink">{t("startup.automation.result")}</span>
          <Badge tone={displayRun.automation.state === "passed" ? "ok" : displayRun.automation.state === "failed" ? "danger" : "brand"}>
            {t(`startup.automation.state.${displayRun.automation.state}`)}
          </Badge>
          {displayRun.automation.exitCode != null && <span className="ml-auto font-mono text-[11px] text-ink-dim">
            exit {displayRun.automation.exitCode}
          </span>}
        </div>
        <div className="mt-2 rounded-lg border border-line/60 bg-terminal px-3 py-2 font-mono text-[11px] text-terminal-ink">
          <div className="mb-1 text-ink-dim">$ {displayRun.automation.command}</div>
          <pre className="max-h-40 overflow-auto whitespace-pre-wrap break-words">{displayRun.automation.commandOutput || displayRun.automation.error || t("startup.automation.waitingOutput")}</pre>
        </div>
      </div>}

      {chartSeries.length > 0 && <div className="mt-3">
        <div className="mb-1 flex flex-wrap items-center gap-3 text-[10px] text-ink-dim">
          <span className="mr-auto font-medium text-ink">
            {t("power.chart.power")} <span className="font-mono font-normal text-ink-dim">({chartAxis.unit})</span>
          </span>
          {chartSeries.map(({ run }, index) => <span key={run.id}><i className={`mr-1 inline-block h-2 w-2 rounded-full ${index === chartSeries.length - 1 ? "bg-brand" : "bg-warn"}`} />{index === chartSeries.length - 1 ? t("startup.current") : t("startup.previous")}</span>)}
        </div>
        <div className="grid grid-cols-[3rem_minmax(0,1fr)] gap-x-2">
          <PowerChartYAxis axis={chartAxis} className="h-36" />
          <svg viewBox="0 0 640 150" preserveAspectRatio="none" role="img" aria-label={`${t("startup.chartAria")} · ${chartAxis.unit}`} className="h-36 w-full rounded-lg border border-line/60 bg-panel2/30">
            <title>{`${t("startup.chartAria")} · ${chartAxis.unit}`}</title>
            {[0.25, 0.5, 0.75].map((ratio) => <line key={ratio} x1="0" x2="640" y1={150 * ratio} y2={150 * ratio} stroke="rgb(var(--c-line))" strokeDasharray="3 5" />)}
            {currentStageBands.map((stage) => <rect key={stage.key}
              x={((stage.start - minX) / (maxX - minX)) * 640}
              width={((stage.end - stage.start) / (maxX - minX)) * 640}
              y="0" height="150" fill={STAGE_COLORS[stage.key]} opacity="0.08" />)}
            <line x1={((-minX) / (maxX - minX)) * 640} x2={((-minX) / (maxX - minX)) * 640} y1="0" y2="150" stroke="rgb(var(--c-danger))" strokeDasharray="4 4" />
            {currentMilestoneMarkers.filter((marker) => marker.xMs >= minX && marker.xMs <= maxX).map((marker) => <line key={marker.key}
              x1={((marker.xMs - minX) / (maxX - minX)) * 640}
              x2={((marker.xMs - minX) / (maxX - minX)) * 640}
              y1="0" y2="150" stroke={MILESTONE_COLORS[marker.key]} strokeWidth="1" strokeDasharray="3 3" vectorEffect="non-scaling-stroke" />)}
            {chartSeries.map(({ run, points }, index) => <polyline key={run.id} fill="none" stroke={index === chartSeries.length - 1 ? "rgb(var(--c-brand))" : "rgb(var(--c-warn))"} strokeWidth="1.8" vectorEffect="non-scaling-stroke"
              points={points.map((point) => `${((point.xMs - minX) / (maxX - minX)) * 640},${146 - (point.powerW / maxY) * 140}`).join(" ")} />)}
          </svg>
        </div>
        {currentStages.length > 0 && <div className="mt-2 flex flex-wrap gap-1.5" aria-label={t("startup.stageLegend")}>
          {currentStages.map((stage) => <span key={stage.key} className="inline-flex items-center gap-1 rounded-full border border-line/70 bg-panel2/55 px-2 py-1 text-[9px] text-ink-dim">
            <i className="h-1.5 w-1.5 rounded-full" style={{ backgroundColor: STAGE_COLORS[stage.key] }} />
            <span>{stage.label}</span>
            <span className="font-mono">{(stage.startedAtMs / 1000).toFixed(2)}s</span>
          </span>)}
        </div>}
      </div>}

      {current && <div className="mt-3 flex flex-wrap justify-end gap-1.5">
        <Button variant="ghost" className="min-h-8 px-2 py-1 text-xs" onClick={() => downloadTaskReport(current)}><Download size={13} />{t("startup.downloadResult")}</Button>
        <Button variant="ghost" className="min-h-8 px-2 py-1 text-xs" onClick={() => downloadSerial(current)}><Download size={13} />{t("startup.download")}</Button>
        {current.capture && <Button variant="ghost" className="min-h-8 px-2 py-1 text-xs" onClick={() => void downloadPowerData(current, "csv").catch((reason) => setError(reason instanceof Error ? reason.message : String(reason)))}><Download size={13} />{t("startup.downloadPowerCsv")}</Button>}
        {current.capture && <Button variant="ghost" className="min-h-8 px-2 py-1 text-xs" onClick={() => void downloadPowerData(current, "ndjson").catch((reason) => setError(reason instanceof Error ? reason.message : String(reason)))}><Download size={13} />{t("startup.downloadPowerNdjson")}</Button>}
      </div>}
      <p className="mt-3 text-[10px] leading-relaxed text-ink-dim"><Activity size={11} className="mr-1 inline" /><Zap size={11} className="mr-1 inline" />{t("startup.note")}</p>
    </Card>
  );
}
