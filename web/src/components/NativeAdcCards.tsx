import type { AdcReading, VoltageAdcReading } from "@/lib/types";
import { useI18n } from "@/lib/i18n";
import { formatPowerMetric } from "@/lib/power";
import { Badge, Card } from "./ui";
import { MeasurementSparkline } from "./PowerSparkline";

export function NativeAdcCards({ readings, stale = false, showTrends = true }: { readings: readonly AdcReading[]; stale?: boolean; showTrends?: boolean }) {
  const { t } = useI18n();
  const native = readings.filter((reading): reading is VoltageAdcReading => reading.kind === "voltage" && reading.role === "native")
    .sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }));
  if (!native.length) return null;
  return <section aria-label={t("adc.native.title")} data-testid="native-adc-cards" className="grid min-w-0 grid-cols-1 gap-3">
    {native.map((reading) => <Card key={`${reading.name}:${reading.signal}`} title={reading.name.toUpperCase()} right={<Badge tone="neutral">{t("power.monitorOnly")}</Badge>} contentClassName="min-w-0 p-3">
      <div data-native-adc={reading.name} className="min-w-0">
        <div className="flex min-w-0 flex-wrap items-baseline justify-between gap-2"><span className="break-all font-mono text-xs text-ink-dim">{reading.signal}</span><span className="font-mono text-lg font-semibold text-brand">{formatPowerMetric(reading.value / 1_000_000, "voltage")}</span></div>
        <p className="mt-1 text-[11px] text-ink-dim">{t("adc.voltage.disclaimer")}{stale ? ` · ${t("snapshot.readOnly")}` : ""}</p>
        {reading.note && <p className="mt-1 break-words text-xs text-ink-dim">{reading.note}</p>}
        <p className="my-2 text-[11px] text-ink-dim">{t("adc.voltage.scale")}</p>
        {showTrends && <MeasurementSparkline mode="voltage" reading={reading} />}
      </div>
    </Card>)}
  </section>;
}
