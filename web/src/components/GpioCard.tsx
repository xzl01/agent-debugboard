import { useId, useMemo, useRef, useState, type ReactNode } from "react";
import { Badge, Card } from "./ui";
import { GpioPhysicalPinoutSvg } from "./GpioPhysicalPinoutSvg";
import type { GpioAction } from "./useGpioPinGesture";
import type { GpioPhysicalLayout, SafeGpio } from "@/lib/types";
import { validateGpioPhysicalJoin } from "@/lib/gpioPhysicalLayout";
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

export function GpioCard({
  gpios,
  physicalLayout,
  layoutError,
  layoutLoading = false,
  onSet,
  disabled = false,
  stale = false,
  workspaceTabs,
}: {
  gpios: SafeGpio[];
  physicalLayout?: GpioPhysicalLayout | null;
  layoutError?: string | null;
  layoutLoading?: boolean;
  onSet: (identifier: string, direction: GpioDirection, value?: number) => Promise<void>;
  disabled?: boolean;
  stale?: boolean;
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
  const joinError = useMemo(() => {
    if (!physicalLayout) return null;
    try {
      validateGpioPhysicalJoin(physicalLayout, gpios);
      return null;
    } catch (cause) {
      return cause instanceof Error ? cause.message : String(cause);
    }
  }, [physicalLayout, gpios]);
  const unavailable = layoutError || joinError;

  const actionLabel = (direction: GpioDirection, value?: number): string =>
    direction === "input"
      ? t("gpio.action.input")
      : value && value > 0
        ? t("gpio.action.outputHigh")
        : t("gpio.action.outputLow");

  const handleGpioAction = (gpioName: string, action: GpioAction) => {
    if (disabled || stale || unavailable || !physicalLayout) return;
    if (pendingRef.current) return;
    const target = gpios.find((g) => g.name === gpioName);
    if (!target) return;
    const cells = physicalLayout.groups.flatMap((group) => group.cells).filter((cell) => cell.controllable && cell.gpioName === target.name);
    if (cells.length !== 1 || cells[0].role === "dedicated" || (action.kind !== "input" && !cells[0].outputCapable)) return;
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
    setPendingPin(target.pin);
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

  return (
    <Card
      headerLeading={workspaceTabs}
      className="min-w-0"
      contentClassName="p-3"
    >
      <div className="mb-2 flex items-center justify-end gap-2">
        {(stale || unavailable || !physicalLayout) && <Badge tone="neutral">{t("snapshot.readOnly")}</Badge>}
        <Badge tone="neutral">{t("gpio.summary")} {outputCount}/{gpios.length}</Badge>
      </div>
      {unavailable ? (
        <div role="alert" className="rounded-lg border border-danger/30 bg-danger/10 p-3 text-xs text-danger">
          <p className="font-medium">{t("pinout.unavailable")}</p>
          <p className="mt-1 break-words">{unavailable}</p>
          <p className="mt-1">{t("pinout.noFallback")}</p>
        </div>
      ) : !physicalLayout ? (
        <p role="status" className="text-sm text-ink-dim">{t(layoutLoading ? "pinout.loading" : "pinout.awaiting")}</p>
      ) : (
        <section className="min-w-0 w-full">
          <GpioPhysicalPinoutSvg layout={physicalLayout} gpios={gpios} pendingPin={pendingPin} instructionsId={instructionsId}
            disabled={disabled || stale} onGpioAction={handleGpioAction} />
          <p id={instructionsId} className="sr-only">
            {HINT_KEYS.map((key, index) => (
              <span key={key}>
                {index > 0 && " · "}
                <span className="inline-block">{t(key)}</span>
              </span>
            ))}
            <span className="mt-1 block">{t("pinout.inputOnly")}</span>
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
