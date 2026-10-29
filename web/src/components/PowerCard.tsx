import { useEffect, useId, useRef, useState } from "react";
import { Activity, ChartLine, Clock3, List, Power, Zap } from "lucide-react";
import { Badge, Button, Card, Toggle } from "./ui";
import type { AdcReading, PdStatus, PowerOutput } from "@/lib/types";
import { isVoltageAdcReading } from "@/lib/adc";
import { useI18n } from "@/lib/i18n";
import { cn } from "@/lib/utils";
import { formatUptime } from "@/lib/utils";
import {
  formatPowerMetric,
  powerRailLabel,
  readingMetric,
  measuredVoltage,
  powerRailRows,
  POWER_STATE_STALE_MS,
} from "@/lib/power";
import { MeasurementSparkline } from "./PowerSparkline";
import { EMPTY_PD_STATUS } from "@/lib/pd";

const POWER_TRENDS_STORAGE_KEY = "linkr-power-trends-expanded";

function initialTrendsExpanded() {
  try {
    return typeof localStorage === "undefined" || localStorage.getItem(POWER_TRENDS_STORAGE_KEY) !== "false";
  } catch {
    return true;
  }
}

export function PowerCard({
  outputs,
  readings,
  pd = EMPTY_PD_STATUS,
  onSet,
  onSetPd = async () => {},
  disabled = false,
  stale = false,
  compact = false,
  onOpenDetails,
  pdInputUpdatedAt,
  vdd5vPending = false,
  vdd5vError,
}: {
  outputs: PowerOutput[];
  readings: readonly AdcReading[];
  pd?: PdStatus;
  onSet: (name: string, on: boolean) => void | Promise<void>;
  onSetPd?: (mode: 0 | 1, voltageMv: number) => Promise<void>;
  disabled?: boolean;
  stale?: boolean;
  compact?: boolean;
  onOpenDetails?: () => void;
  pdInputUpdatedAt?: number | null;
  vdd5vPending?: boolean;
  vdd5vError?: string | null;
}) {
  const { t } = useI18n();
  const [trendsExpanded, setTrendsExpanded] = useState(initialTrendsExpanded);
  const showTrends = !compact && trendsExpanded;
  const [clockMs, setClockMs] = useState(() => Date.now());
  const [pdMode, setPdMode] = useState<0 | 1>(pd.mode);
  const pdVoltageKnown = Number.isFinite(pd.voltage_mv) && pd.voltage_mv > 0;
  const [pdVoltageDraft, setPdVoltageDraft] = useState(() => pdVoltageKnown ? String(pd.voltage_mv) : "");
  const [pdVoltageTouched, setPdVoltageTouched] = useState(false);
  const pdVoltageErrorId = useId();
  const [pdBusy, setPdBusy] = useState(false);
  const [pdError, setPdError] = useState<string | null>(null);
  const [railError, setRailError] = useState<string | null>(null);
  const [railTimers, setRailTimers] = useState(new Map<string, { startedAtMs: number; approximate: boolean }>());
  const railTimersRef = useRef(railTimers);
  const observedRailsRef = useRef(new Set<string>());
  const fixedProfiles = pd.fixed_profiles;
  const ppsRanges = pd.pps_ranges;
  const ppsMinMv = ppsRanges.length > 0 ? Math.min(...ppsRanges.map((range) => range.min_voltage_mv)) : 0;
  const ppsMaxMv = ppsRanges.length > 0 ? Math.max(...ppsRanges.map((range) => range.max_voltage_mv)) : 0;
  const ppsStepMv = ppsRanges[0]?.step_mv ?? 100;
  const hasPdCapabilities = pd.capabilities_valid && (fixedProfiles.length > 0 || ppsRanges.length > 0);
  const pdControlsDisabled = disabled || stale || pdBusy || !pd.available || !pd.pd || pd.last_error !== 0 || !hasPdCapabilities;
  const modeSupported = pdMode === 0 ? fixedProfiles.length > 0 : ppsRanges.length > 0;
  const draftVoltageMv = Number(pdVoltageDraft);
  const pdVoltageMv = pdMode === 0
    ? fixedProfiles.find((profile) => profile.voltage_mv === draftVoltageMv)?.voltage_mv ?? 0
    : draftVoltageMv;
  const voltageValid = pdMode === 0 ? pdVoltageMv > 0 : (
    pdVoltageDraft.trim() !== "" && Number.isSafeInteger(pdVoltageMv) &&
    pdVoltageMv <= pd.board_max_voltage_mv && ppsRanges.some((range) =>
      pdVoltageMv >= range.min_voltage_mv && pdVoltageMv <= range.max_voltage_mv &&
      range.step_mv > 0 && (pdVoltageMv - range.min_voltage_mv) % range.step_mv === 0)
  );
  const pdInput = readings.filter(isVoltageAdcReading).find((reading) => reading.role === "pd_input");
  const measurementNow = Math.max(clockMs, Date.now());
  const pdInputFresh = !stale && pdInput && (pdInputUpdatedAt === undefined || (pdInputUpdatedAt !== null && measurementNow - pdInputUpdatedAt <= POWER_STATE_STALE_MS && measurementNow >= pdInputUpdatedAt));
  const rows = powerRailRows(outputs, readings);

  useEffect(() => {
    const now = Date.now();
    const timers = new Map(railTimersRef.current);
    const reported = new Set(outputs.map((output) => output.name));
    for (const name of observedRailsRef.current) {
      if (!reported.has(name)) { observedRailsRef.current.delete(name); timers.delete(name); }
    }
    for (const output of outputs) {
      const name = output.name;
      const firstObservation = !observedRailsRef.current.has(name);
      observedRailsRef.current.add(name);
      if (output.state === "on") {
        if (!timers.has(name)) {
          timers.set(name, { startedAtMs: now, approximate: firstObservation });
        }
      } else {
        timers.delete(name);
      }
    }
    railTimersRef.current = timers;
    setRailTimers(timers);
    setClockMs(now);
  }, [outputs]);

  useEffect(() => {
    const timer = window.setInterval(() => {
      setClockMs(Date.now());
    }, 1000);
    return () => window.clearInterval(timer);
  }, []);

  useEffect(() => {
    try {
      localStorage.setItem(POWER_TRENDS_STORAGE_KEY, String(trendsExpanded));
    } catch {
      // Preferences are best-effort; blocked storage must not hide controls.
    }
  }, [trendsExpanded]);

  useEffect(() => {
    setPdMode(pd.mode);
    setPdVoltageDraft(pd.voltage_mv > 0 ? String(pd.voltage_mv) : "");
    setPdVoltageTouched(false);
  }, [pd.mode, pd.voltage_mv]);

  const applyPd = async () => {
    setPdVoltageTouched(true);
    if (pdControlsDisabled || !modeSupported || !voltageValid) return;
    setPdBusy(true);
    setPdError(null);
    try {
      await onSetPd(pdMode, pdVoltageMv);
    } catch (error) {
      setPdError(error instanceof Error ? error.message : String(error));
    } finally {
      setPdBusy(false);
    }
  };

  const setRail = async (name: string, on: boolean) => {
    setRailError(null);
    try {
      await onSet(name, on);
    } catch (cause) {
      // A shared vdd_5v action supplies its error as well as pending state, so
      // a retry from the header cannot leave an obsolete drawer-only error.
      if (name !== "vdd_5v" || vdd5vError === undefined) {
        setRailError(cause instanceof Error ? cause.message : String(cause));
      }
    }
  };

  return (
    <Card
      title={t("power.combined.title")}
      subtitle={compact ? undefined : t("power.combined.subtitle")}
      icon={compact ? undefined : Power}
      className={compact ? "rounded-none border-0 shadow-none" : undefined}
      headerClassName={compact ? "min-h-11 px-3 py-2" : undefined}
      contentClassName={compact ? "p-3" : undefined}
      right={
        <div className="flex max-w-full flex-wrap items-center justify-end gap-1.5">
          {stale && <Badge tone="neutral">{t("snapshot.readOnly")}</Badge>}
          {!compact && <button
            type="button"
            aria-pressed={trendsExpanded}
            aria-expanded={showTrends}
            onClick={() => setTrendsExpanded((expanded) => !expanded)}
            className="inline-flex min-h-8 cursor-pointer items-center gap-1.5 rounded-lg border border-line/70 bg-panel2/60 px-2.5 text-xs font-medium text-ink-dim transition-colors hover:border-brand/30 hover:text-ink focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40"
          >
            {trendsExpanded ? <List size={13} /> : <ChartLine size={13} />}
            {t(trendsExpanded ? "power.chart.compact" : "power.chart.expand")}
          </button>}
        </div>
      }
    >
      <section data-testid="pd-input-card" className="mb-3 min-w-0 rounded-lg border border-line/60 bg-panel2/40 p-3">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div>
            <div className="flex items-center gap-2 text-sm font-medium text-ink">
              {t("pd.input.title")}
              <Badge tone={pd.available ? "ok" : "warn"}>
                {pd.available ? t("pd.available") : t("pd.unavailable")}
              </Badge>
            </div>
            <div className="mt-0.5 text-xs text-ink-dim" data-testid="pd-live-status">
              {pdVoltageKnown ? t("pd.status", {
                protocol: pd.protocol.toUpperCase(),
                voltage: pd.voltage_mv > 0 ? (pd.voltage_mv / 1000).toFixed(1) : "—",
                current: pd.max_current_ma,
              }) : t(hasPdCapabilities ? "pd.requestVoltageUnknownHint" : "pd.voltageUnknown")}
            </div>
          </div>
          <div className="flex flex-wrap items-center justify-end gap-2">
            <select
              aria-label={t("pd.mode")}
              value={pdMode}
              disabled={pdControlsDisabled}
              onChange={(event) => {
                const mode = Number(event.target.value) === 1 ? 1 : 0;
                setPdMode(mode);
                setPdVoltageTouched(false);
                if (mode === 1 && !ppsRanges.some((range) => draftVoltageMv >= range.min_voltage_mv && draftVoltageMv <= range.max_voltage_mv)) {
                  setPdVoltageDraft(String(ppsMinMv));
                }
              }}
              className="min-h-9 rounded-lg border border-line bg-panel px-2 text-sm text-ink"
            >
              <option value={0} disabled={fixedProfiles.length === 0}>{t("pd.fixed")}</option>
              <option value={1} disabled={ppsRanges.length === 0}>{t("pd.pps")}</option>
            </select>
            {pdMode === 0 ? (
              <select
                aria-label={t("pd.voltage")}
                value={pdVoltageMv}
                disabled={pdControlsDisabled || !modeSupported}
                onChange={(event) => setPdVoltageDraft(event.target.value)}
                className="min-h-9 rounded-lg border border-line bg-panel px-2 text-sm text-ink"
              >
                {pdVoltageMv === 0 && <option value={0} disabled>{pdVoltageKnown ? `${pd.voltage_mv / 1000} V` : t("pd.requestVoltageUnknown")}</option>}
                {fixedProfiles.map((profile) => (
                  <option key={profile.voltage_mv} value={profile.voltage_mv}>
                    {profile.voltage_mv / 1000} V · {profile.max_current_ma} mA
                  </option>
                ))}
              </select>
            ) : (
              <label className="inline-flex items-center gap-1.5 text-xs text-ink-dim">
                <input
                  aria-label={`${t("pd.voltage")} (mV)`}
                  aria-invalid={!voltageValid}
                  aria-describedby={pdVoltageTouched && !voltageValid ? pdVoltageErrorId : undefined}
                  type="number"
                  min={ppsMinMv}
                  max={Math.min(ppsMaxMv, pd.board_max_voltage_mv)}
                  step={ppsStepMv}
                  value={pdVoltageDraft}
                  disabled={pdControlsDisabled || !modeSupported}
                  onChange={(event) => setPdVoltageDraft(event.target.value)}
                  onBlur={() => setPdVoltageTouched(true)}
                  className="min-h-9 w-28 rounded-lg border border-line bg-panel px-2 font-mono text-sm text-ink"
                />
                <span>mV</span>
              </label>
            )}
            <Button type="button" disabled={pdControlsDisabled || !modeSupported || !voltageValid} onClick={() => void applyPd()}>
              {pdBusy ? t("pd.applying") : t("pd.apply")}
            </Button>
          </div>
        </div>
        {pdMode === 1 && pdVoltageTouched && !voltageValid && (
          <p id={pdVoltageErrorId} role="alert" className="mt-2 text-xs text-danger">{t("pd.invalidVoltage")}</p>
        )}
        {pd.capabilities_valid && (
          <p className="mt-2 text-[11px] text-ink-dim">
            {t("pd.capabilities", {
              fixed: fixedProfiles.map((profile) => `${profile.voltage_mv / 1000}V/${profile.max_current_ma}mA`).join(", ") || "—",
              pps: ppsRanges.map((range) => `${range.min_voltage_mv / 1000}–${range.max_voltage_mv / 1000}V/${range.max_current_ma}mA`).join(", ") || "—",
            })}
          </p>
        )}
        {pdError && <p className="mt-2 text-xs text-danger">{pdError}</p>}
        <div className="mt-3 min-w-0 border-t border-line/50 pt-3" data-testid="pd-input-measurement">
          <div className="flex min-w-0 flex-wrap items-baseline justify-between gap-2">
            <span className="text-xs text-ink-dim">{t("pd.input.measured")}</span>
            <span className="font-mono text-xl font-semibold text-brand">{pdInputFresh ? formatPowerMetric(pdInput.value / 1_000_000, "voltage") : "—"}</span>
          </div>
          <p className="mt-1 text-[11px] text-ink-dim">{pdInput ? `${pdInput.name} · ${pdInput.signal}` : t("pd.input.unknown")}{pdInput && !pdInputFresh ? ` · ${t("snapshot.readOnly")}` : ""}</p>
          <p className="mt-1 text-[11px] text-ink-dim">{t("pd.input.sourceHint")}</p>
          {pdInputFresh && showTrends && <div className="mt-2"><MeasurementSparkline mode="voltage" reading={pdInput} /></div>}
          {pdInput && (pdInput.raw != null || pdInput.mv != null) && <details className="mt-2 text-xs text-ink-dim"><summary className="cursor-pointer">{t("pd.input.diagnostics")}</summary><p className="mt-1 break-all font-mono">raw: {pdInput.raw ?? "—"} · pad: {pdInput.mv == null ? "—" : `${pdInput.mv} mV`}</p></details>}
        </div>
      </section>
      {rows.length === 0 ? (
        <p className="text-sm text-ink-dim">{t("power.combined.none")}</p>
      ) : (
        <ul className="divide-y divide-line/50">
          {rows.map(({ name, output, reading }) => {
            const on = output?.state === "on";
            const locked = output ? !output.controllable || output.state === "locked" : false;
            const currentValue = reading ? readingMetric(reading, "current") : null;
            const voltageValue = reading ? measuredVoltage(reading) : null;
            const powerValue = reading ? readingMetric(reading, "power") : null;
            const onTiming = on ? railTimers.get(name) : undefined;
            return (
               <li
                 key={name}
                 data-power-rail={name}
                className={cn(
                  "grid grid-cols-[minmax(0,1fr)_auto] gap-x-3 gap-y-1",
                  showTrends ? "py-3" : "py-2.5"
                )}
              >
                <div className="min-w-0">
                  <div className="flex flex-wrap items-center gap-2">
                    <span className="font-medium text-ink">{powerRailLabel(name)}</span>
                    {locked && <Badge tone="warn">{t("power.locked")}</Badge>}
                    {!output && <Badge tone="neutral">{t("power.monitorOnly")}</Badge>}
                  </div>
                  <div className="break-all text-xs text-ink-dim">
                    {output?.signal ?? reading?.signal ?? name}
                    {output && !locked && (
                      <span className="ml-2 text-ink-dim">
                        {stale ? t("snapshot.lastPrefix") : ""}{on ? t("power.on") : output.state === "off" ? t("power.off") : output.state || "—"}
                        {output.value == null ? "" : ` · ${output.value}`}
                      </span>
                    )}
                  </div>
                  {!compact && onTiming && (
                    <div
                      className="mt-1 inline-flex items-center gap-1 text-[10px] text-ink-dim"
                      title={t("power.onDurationHint")}
                    >
                      <Clock3 size={11} />
                      {t("power.onDuration")} {onTiming.approximate ? "≥ " : ""}{formatUptime(Math.floor((clockMs - onTiming.startedAtMs) / 1000))}
                    </div>
                  )}
                </div>
                <div className="flex items-center gap-3">
                  {reading && <div className="min-w-[82px] text-right" data-power-measurements="">
                    <div className="inline-flex items-center gap-1 font-mono text-sm font-medium text-brand">
                      <Activity size={13} />
                      {currentValue == null ? "—" : formatPowerMetric(currentValue, "current")}
                    </div>
                    <div className="flex items-center justify-end gap-1 font-mono text-[11px] font-medium text-ok">
                      <span aria-hidden="true">V</span>
                      {voltageValue == null ? "—" : formatPowerMetric(voltageValue, "voltage")}
                    </div>
                    <div className="flex items-center justify-end gap-1 font-mono text-[11px] font-medium text-warn">
                      <Zap size={11} />
                      {powerValue == null ? "—" : formatPowerMetric(powerValue, "power")}
                    </div>
                  </div>}
                  {output && (
                    <Toggle
                      aria-label={t("power.railSwitch", { rail: powerRailLabel(name) })}
                      checked={on}
                      disabled={disabled || stale || locked || !["on", "off"].includes(output.state) || (name === "vdd_5v" && vdd5vPending)}
                      aria-busy={name === "vdd_5v" && vdd5vPending}
                      onChange={(value) => { void setRail(output.name, value); }}
                    />
                  )}
                </div>
                {!reading && <p data-measurement-unavailable="" className="col-span-2 text-[11px] leading-4 text-ink-dim">{t("power.measurementUnavailable")}</p>}
                {reading && !reading.power_enabled && (
                  <div className="col-span-2 text-[11px] text-warn">{t("adc.disabled")}</div>
                )}
                {reading && showTrends && (
                  <div className="col-span-2 mt-2 min-w-0">
                    <MeasurementSparkline mode="power" reading={reading} />
                  </div>
                )}
              </li>
            );
          })}
        </ul>
      )}
      {(vdd5vError || railError) && <p role="alert" className="mt-3 text-xs text-danger">{vdd5vError || railError}</p>}
      {compact && onOpenDetails && (
        <Button
          type="button"
          variant="ghost"
          className="mt-3 min-h-8 px-0 py-1 text-xs text-brand hover:bg-transparent"
          onClick={onOpenDetails}
        >
          {t("quick.power.details")}
        </Button>
      )}
    </Card>
  );
}
