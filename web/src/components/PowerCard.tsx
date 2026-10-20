import { useEffect, useId, useRef, useState } from "react";
import { Activity, ChartLine, Clock3, List, Power, Zap } from "lucide-react";
import { Badge, Button, Card, Toggle } from "./ui";
import type { AdcReading, PdStatus, PowerOutput } from "@/lib/types";
import { isCurrentAdcReading, isVoltageAdcReading } from "@/lib/adc";
import { useI18n } from "@/lib/i18n";
import { cn } from "@/lib/utils";
import { formatUptime } from "@/lib/utils";
import {
  formatPowerMetric,
  powerRailLabel,
  readingMetric,
  measuredVoltage,
  POWER_SWITCH_RAILS,
} from "@/lib/power";
import { MeasurementSparkline } from "./PowerSparkline";
import { EMPTY_PD_STATUS } from "@/lib/pd";

const POWER_TRENDS_STORAGE_KEY = "linkr-power-trends-expanded";

function initialTrendsExpanded() {
  if (typeof localStorage === "undefined") return true;
  return localStorage.getItem(POWER_TRENDS_STORAGE_KEY) !== "false";
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
}: {
  outputs: PowerOutput[];
  readings: readonly AdcReading[];
  pd?: PdStatus;
  onSet: (name: string, on: boolean) => void;
  onSetPd?: (mode: 0 | 1, voltageMv: number) => Promise<void>;
  disabled?: boolean;
  stale?: boolean;
  compact?: boolean;
  onOpenDetails?: () => void;
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
  const railTimersRef = useRef(new Map<string, { startedAtMs: number; approximate: boolean }>());
  const observedRailsRef = useRef(new Set<string>());
  const currentReadings = readings.filter(isCurrentAdcReading);
  const fixedProfiles = pd.fixed_profiles;
  const ppsRanges = pd.pps_ranges;
  const ppsMinMv = ppsRanges.length > 0 ? Math.min(...ppsRanges.map((range) => range.min_voltage_mv)) : 0;
  const ppsMaxMv = ppsRanges.length > 0 ? Math.max(...ppsRanges.map((range) => range.max_voltage_mv)) : 0;
  const ppsStepMv = ppsRanges[0]?.step_mv ?? 100;
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
  const voltageReadings = readings.filter(isVoltageAdcReading);
  const rows = POWER_SWITCH_RAILS.map((name) => ({
    name,
    output: outputs.find((output) => output.name === name),
    reading: currentReadings.find((reading) => reading.name === name),
  })).filter(({ output, reading }) => output || reading);

  useEffect(() => {
    const now = Date.now();
    for (const name of POWER_SWITCH_RAILS) {
      const output = outputs.find((item) => item.name === name);
      if (!output) continue;
      const firstObservation = !observedRailsRef.current.has(name);
      observedRailsRef.current.add(name);
      if (output.state === "on") {
        if (!railTimersRef.current.has(name)) {
          railTimersRef.current.set(name, { startedAtMs: now, approximate: firstObservation });
        }
      } else {
        railTimersRef.current.delete(name);
      }
    }
    setClockMs(now);
  }, [outputs]);

  useEffect(() => {
    const timer = window.setInterval(() => {
      if (railTimersRef.current.size > 0) setClockMs(Date.now());
    }, 1000);
    return () => window.clearInterval(timer);
  }, []);

  useEffect(() => {
    localStorage.setItem(POWER_TRENDS_STORAGE_KEY, String(trendsExpanded));
  }, [trendsExpanded]);

  useEffect(() => {
    setPdMode(pd.mode);
    setPdVoltageDraft(pd.voltage_mv > 0 ? String(pd.voltage_mv) : "");
    setPdVoltageTouched(false);
  }, [pd.mode, pd.voltage_mv]);

  const applyPd = async () => {
    setPdVoltageTouched(true);
    if (disabled || stale || pdBusy || !pd.available || !pdVoltageKnown || !modeSupported || !voltageValid) return;
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
      <section className="mb-3 rounded-lg border border-line/60 bg-panel2/40 p-3">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div>
            <div className="flex items-center gap-2 text-sm font-medium text-ink">
              {t("pd.title")}
              <Badge tone={pd.available ? "ok" : "warn"}>
                {pd.available ? t("pd.available") : t("pd.unavailable")}
              </Badge>
            </div>
            <div className="mt-0.5 text-xs text-ink-dim" data-testid="pd-live-status">
              {pdVoltageKnown ? t("pd.status", {
                protocol: pd.protocol.toUpperCase(),
                voltage: pd.voltage_mv > 0 ? (pd.voltage_mv / 1000).toFixed(1) : "—",
                current: pd.max_current_ma,
              }) : t("pd.voltageUnknown")}
            </div>
          </div>
          <div className="flex flex-wrap items-center justify-end gap-2">
            <select
              aria-label={t("pd.mode")}
              value={pdMode}
              disabled={disabled || stale || pdBusy || !pdVoltageKnown}
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
              <option value={0}>{t("pd.fixed")}</option>
              {ppsRanges.length > 0 && <option value={1}>{t("pd.pps")}</option>}
            </select>
            {pdMode === 0 ? (
              <select
                aria-label={t("pd.voltage")}
                value={pdVoltageMv}
                disabled={disabled || stale || pdBusy || !pdVoltageKnown || fixedProfiles.length === 0}
                onChange={(event) => setPdVoltageDraft(event.target.value)}
                className="min-h-9 rounded-lg border border-line bg-panel px-2 text-sm text-ink"
              >
                {pdVoltageMv === 0 && <option value={0} disabled>{pdVoltageKnown ? `${pd.voltage_mv / 1000} V` : t("pd.voltageUnknown")}</option>}
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
                  disabled={disabled || stale || pdBusy || !pdVoltageKnown}
                  onChange={(event) => setPdVoltageDraft(event.target.value)}
                  onBlur={() => setPdVoltageTouched(true)}
                  className="min-h-9 w-28 rounded-lg border border-line bg-panel px-2 font-mono text-sm text-ink"
                />
                <span>mV</span>
              </label>
            )}
            <Button type="button" disabled={disabled || stale || pdBusy || !pd.available || !pdVoltageKnown || !modeSupported || !voltageValid} onClick={() => void applyPd()}>
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
            const onTiming = on ? railTimersRef.current.get(name) : undefined;
            return (
              <li
                key={name}
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
                  <div className="text-xs text-ink-dim">
                    {output?.signal ?? reading?.signal ?? `GP${output?.gp ?? "?"}`}
                    {output?.value != null && !locked && (
                      <span className="ml-2 text-ink-dim">
                        {stale ? t("snapshot.lastPrefix") : ""}{on ? t("power.on") : t("power.off")} · {output.value}
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
                  <div className="min-w-[82px] text-right">
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
                  </div>
                  {output && (
                    <Toggle
                      aria-label={t("power.railSwitch", { rail: powerRailLabel(name) })}
                      checked={on}
                      disabled={disabled || locked}
                      onChange={(value) => onSet(output.name, value)}
                    />
                  )}
                </div>
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
      {voltageReadings.length > 0 && (
        <section className="mt-4 border-t border-line/50 pt-4">
          <div className="flex flex-wrap items-center gap-2">
            <h3 className="text-sm font-medium text-ink">{t("adc.voltage.title")}</h3>
            <Badge tone="neutral">{t("power.monitorOnly")}</Badge>
          </div>
          <p className="mt-1 text-xs text-ink-dim">{t("adc.voltage.disclaimer")}</p>
          {showTrends && <p className="mt-1 text-xs text-ink-dim">{t("adc.voltage.scale")}</p>}
          <ul className="mt-3 grid grid-cols-1 gap-3 sm:grid-cols-2">
            {voltageReadings.map((reading) => (
              <li key={JSON.stringify([reading.name, reading.signal])} className="min-w-0 rounded-lg border border-line/60 bg-panel2/40 p-3">
                <div className="flex flex-wrap items-start justify-between gap-2">
                  <div className="min-w-0">
                    <h4 className="break-words font-mono text-sm font-medium text-ink">{reading.name}</h4>
                    <p className="mt-0.5 break-words font-mono text-xs text-ink-dim">{reading.signal}</p>
                  </div>
                  <div className="shrink-0 font-mono text-sm font-medium text-brand">
                    {formatPowerMetric(reading.value / 1_000_000, "voltage")}
                  </div>
                </div>
                {reading.note && <p className="mt-1 break-words text-xs text-ink-dim">{reading.note}</p>}
                {showTrends && (
                  <div className="mt-2">
                    <MeasurementSparkline mode="voltage" reading={reading} />
                  </div>
                )}
              </li>
            ))}
          </ul>
        </section>
      )}
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
