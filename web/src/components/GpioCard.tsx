import { useEffect, useId, useRef, useState, type ReactNode } from "react";
import { Pin } from "lucide-react";
import { Badge, Button, Card, Toggle } from "./ui";
import { GpioPinoutSvg } from "./GpioPinoutSvg";
import type { GpioAction } from "./useGpioPinGesture";
import type { SafeGpio } from "@/lib/types";
import { useI18n } from "@/lib/i18n";

type GpioDirection = "input" | "output";

interface GpioSuccess {
  readonly name: string;
  readonly direction: GpioDirection;
  readonly value?: number;
}

function assertNever(value: never): never {
  throw new Error(`unexpected GPIO action: ${JSON.stringify(value)}`);
}

const HINT_KEYS = [
  "gpio.hint.short",
  "gpio.hint.double",
  "gpio.hint.hold",
  "gpio.hint.keys",
] as const;

function GpioRow({
  gpio,
  onApply,
  disabled,
  stale,
}: {
  gpio: SafeGpio;
  onApply: (identifier: string, direction: GpioDirection, value?: number) => void;
  disabled?: boolean;
  stale?: boolean;
}) {
  const { t } = useI18n();
  const actualDirection = gpio.direction === "output" ? "output" : "input";
  const [value, setValue] = useState(gpio.value > 0);

  useEffect(() => {
    setValue(gpio.value > 0);
  }, [gpio.value]);

  return (
    <li className="flex h-9 items-center justify-between border-b border-line/50 last:border-b-0">
      <div className="flex w-full items-center justify-between gap-3">
        <div className="min-w-0">
          <div className="flex items-center gap-2">
            <span className="font-medium text-ink">{gpio.name}</span>
            <span className="text-[10px] text-ink-dim">
              {actualDirection === "output" ? t("gpio.output") : t("gpio.input")}
            </span>
          </div>
        </div>
        {actualDirection === "output" ? (
          <div className="flex items-center gap-2 text-xs text-ink-dim">
            <span>{value ? t("gpio.high") : t("gpio.low")}</span>
            <Toggle
              checked={value}
              disabled={disabled}
              onChange={(next) => {
                setValue(next);
                onApply(gpio.name, "output", next ? 1 : 0);
              }}
            />
          </div>
        ) : (
          <span className="text-xs text-ink-dim">
            {t(stale ? "snapshot.last" : "gpio.current")}: {gpio.value > 0 ? t("gpio.high") : t("gpio.low")}
          </span>
        )}
      </div>
    </li>
  );
}

export function GpioCard({
  gpios,
  onSet,
  disabled = false,
  stale = false,
  compact = false,
  limit,
  onOpenDetails,
  workspaceTabs,
}: {
  gpios: SafeGpio[];
  onSet: (identifier: string, direction: GpioDirection, value?: number) => Promise<void>;
  disabled?: boolean;
  stale?: boolean;
  compact?: boolean;
  limit?: number;
  onOpenDetails?: () => void;
  workspaceTabs?: ReactNode;
}) {
  const { t } = useI18n();
  const instructionsId = useId();
  const [error, setError] = useState<string | null>(null);
  const [success, setSuccess] = useState<GpioSuccess | null>(null);
  const [pendingPin, setPendingPin] = useState<number | null>(null);
  // Synchronous lock: a second gesture dispatched before React re-renders the
  // pending state is dropped here instead of racing the in-flight request.
  const pendingRef = useRef(false);

  const actionLabel = (direction: GpioDirection, value?: number): string =>
    direction === "input"
      ? t("gpio.action.input")
      : value && value > 0
        ? t("gpio.action.outputHigh")
        : t("gpio.action.outputLow");

  const handleGpioAction = (pin: number, action: GpioAction) => {
    if (disabled || stale) return;
    if (pendingRef.current) return;
    const target = gpios.find((g) => g.pin === pin);
    if (!target) return;
    let direction: GpioDirection;
    let value: number | undefined;
    switch (action.kind) {
      case "input":
        direction = "input";
        value = undefined;
        break;
      case "outputLow":
        direction = "output";
        value = 0;
        break;
      case "outputHigh":
        direction = "output";
        value = 1;
        break;
      default:
        assertNever(action);
    }
    pendingRef.current = true;
    setError(null);
    setSuccess(null);
    setPendingPin(pin);
    const request =
      value === undefined
        ? onSet(target.name, direction)
        : onSet(target.name, direction, value);
    void request
      .then(() => {
        setSuccess({ name: target.name, direction, value });
      })
      .catch((cause: unknown) => {
        setError(cause instanceof Error ? cause.message : String(cause));
      })
      .finally(() => {
        pendingRef.current = false;
        setPendingPin(null);
      });
  };

  const outputCount = gpios.filter((g) => g.direction === "output").length;

  if (compact) {
    return (
      <Card
        title={t("gpio.title")}
        right={
          <div className="flex items-center gap-2">
            <span className="text-[10px] text-ink-dim">{t("quick.commonPins")}</span>
            {stale && <Badge tone="neutral">{t("snapshot.readOnly")}</Badge>}
          </div>
        }
        className="rounded-none border-0 shadow-none"
        headerClassName="min-h-11 px-3 py-2"
        contentClassName="flex min-h-0 flex-col p-3"
      >
        {gpios.length === 0 ? (
          <p className="text-sm text-ink-dim">{t("gpio.none")}</p>
        ) : (
          <ul className="min-h-0 overflow-y-auto">
            {gpios.slice(0, limit).map((g) => (
              <GpioRow key={g.name + g.pin} gpio={g} onApply={onSet} disabled={disabled} stale={stale} />
            ))}
          </ul>
        )}
        {gpios.length > 0 && (
          <div className="mt-2 flex flex-wrap items-center justify-between gap-2">
            <p className="text-[11px] text-ink-dim">
              {t("quick.managePins", { shown: Math.min(limit ?? gpios.length, gpios.length), total: gpios.length })}
            </p>
            {onOpenDetails && (
              <Button variant="ghost" className="min-h-7 px-0 py-1 text-[11px] text-brand hover:bg-transparent" onClick={onOpenDetails}>
                {t("quick.manageControls")}
              </Button>
            )}
          </div>
        )}
      </Card>
    );
  }

  return (
    <Card
      title={workspaceTabs ? undefined : t("gpio.title")}
      subtitle={workspaceTabs ? undefined : t("gpio.subtitle")}
      icon={Pin}
      headerLeading={workspaceTabs}
      right={
        <div className="flex items-center gap-2">
          {stale && <Badge tone="neutral">{t("snapshot.readOnly")}</Badge>}
          <Badge tone="neutral">
            {t("gpio.summary")} {outputCount}/{gpios.length}
          </Badge>
        </div>
      }
    >
      {gpios.length === 0 ? (
        <p className="text-sm text-ink-dim">{t("gpio.none")}</p>
      ) : (
        <section className="min-w-0 w-full max-w-[300px] rounded-xl border border-line/60 bg-panel2/35 p-3">
          <div className="overflow-hidden rounded-lg border border-line/50 bg-panel/70">
            <GpioPinoutSvg
              gpios={gpios}
              variant="gpio"
              gpioPendingPin={pendingPin}
              gpioInstructionsId={instructionsId}
              onGpioAction={handleGpioAction}
            />
          </div>
          <p id={instructionsId} className="mt-2 text-[11px] leading-4 text-ink-dim">
            {HINT_KEYS.map((key, index) => (
              <span key={key}>
                {index > 0 && " · "}
                <span className="inline-block">{t(key)}</span>
              </span>
            ))}
          </p>
        </section>
      )}

      {error && (
        <div
          role="alert"
          className="mt-3 rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger"
        >
          {error}
        </div>
      )}
      {success && (
        <div
          role="status"
          aria-live="polite"
          className="mt-3 rounded-lg border border-ok/30 bg-ok/10 px-3 py-2 text-xs text-ok"
        >
          {t("gpio.applied", {
            action: actionLabel(success.direction, success.value),
            name: success.name,
          })}
        </div>
      )}
    </Card>
  );
}
