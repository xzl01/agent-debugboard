import {
  forwardRef,
  type ButtonHTMLAttributes,
  type HTMLAttributes,
  type ReactNode,
} from "react";
import type { LucideIcon } from "lucide-react";
import { cn } from "@/lib/utils";

export function Card({
  title,
  subtitle,
  icon: Icon,
  right,
  headerLeading,
  children,
  className,
  contentClassName,
}: {
  title?: string;
  subtitle?: string;
  icon?: LucideIcon;
  right?: ReactNode;
  /** Replaces the standard icon/title block in the card header. */
  headerLeading?: ReactNode;
  children?: ReactNode;
  className?: string;
  contentClassName?: string;
}) {
  return (
    <section
      className={cn(
        "flex min-h-0 flex-col rounded-2xl border border-line/70 bg-panel shadow-sm transition-colors duration-200",
        className
      )}
    >
      {(title || right || headerLeading) && (
        <header className="flex flex-wrap items-center justify-between gap-3 border-b border-line/60 px-4 py-3">
          {headerLeading ? (
            <div className="min-w-0 max-w-full">{headerLeading}</div>
          ) : (
            <div className="flex min-w-0 items-center gap-3">
              {Icon && (
                <span className="grid h-8 w-8 shrink-0 place-items-center rounded-lg bg-brand/10 text-brand">
                  <Icon size={16} />
                </span>
              )}
              <div className="min-w-0">
                {title && (
                  <h2 className="truncate text-sm font-semibold tracking-wide text-ink">
                    {title}
                  </h2>
                )}
                {subtitle && <p className="truncate text-xs text-ink-dim">{subtitle}</p>}
              </div>
            </div>
          )}
          {right && <div className="max-w-full shrink-0">{right}</div>}
        </header>
      )}
      <div className={cn("min-h-0 flex-1 p-4", contentClassName)}>{children}</div>
    </section>
  );
}

type ButtonVariant = "default" | "primary" | "danger" | "ghost";

export const Button = forwardRef<
  HTMLButtonElement,
  ButtonHTMLAttributes<HTMLButtonElement> & { variant?: ButtonVariant }
>(function Button({
  variant = "default",
  className,
  ...props
}, ref) {
  const variants: Record<ButtonVariant, string> = {
    default:
      "bg-panel2 text-ink hover:bg-line/50 border border-line/70 hover:border-line",
    primary:
      "bg-brand text-on-brand hover:bg-brand/85 border border-brand/40 shadow-sm shadow-brand/20",
    danger:
      "bg-danger text-on-danger hover:bg-danger/90 border border-danger/40 shadow-sm shadow-danger/20",
    ghost:
      "bg-transparent text-ink-dim hover:bg-panel2 hover:text-ink border border-transparent",
  };
  return (
    <button
      ref={ref}
      className={cn(
        "inline-flex min-h-10 items-center justify-center gap-2 rounded-xl px-3 py-2 text-sm font-medium transition-all duration-150 active:scale-[0.97] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40 disabled:cursor-not-allowed disabled:opacity-50 disabled:active:scale-100",
        variants[variant],
        className
      )}
      {...props}
    />
  );
});

export function Toggle({
  checked,
  onChange,
  disabled,
}: {
  checked: boolean;
  onChange: (next: boolean) => void;
  disabled?: boolean;
}) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      disabled={disabled}
      onClick={() => onChange(!checked)}
      className={cn(
        "relative h-6 w-11 shrink-0 rounded-full transition-colors duration-200 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40",
        checked ? "bg-ok" : "bg-line",
        disabled ? "cursor-not-allowed opacity-40" : "cursor-pointer"
      )}
    >
      <span
        className={cn(
          "absolute left-0.5 top-0.5 h-5 w-5 rounded-full bg-white shadow transition-transform duration-200",
          checked ? "translate-x-5" : "translate-x-0"
        )}
      />
    </button>
  );
}

type Tone = "neutral" | "ok" | "warn" | "danger" | "brand";

export function Badge({
  children,
  tone = "neutral",
  className,
  ...props
}: HTMLAttributes<HTMLSpanElement> & {
  readonly children: ReactNode;
  readonly tone?: Tone;
}) {
  const tones: Record<Tone, string> = {
    neutral: "bg-panel2 text-ink-dim border-line/70",
    ok: "bg-ok/15 text-ink border-ok/30 [&>svg]:text-ok",
    warn: "bg-warn/15 text-ink border-warn/30 [&>svg]:text-warn",
    danger: "bg-danger/15 text-ink border-danger/30 [&>svg]:text-danger",
    brand: "bg-brand/15 text-ink border-brand/30 [&>svg]:text-brand",
  };
  return (
    <span
      className={cn(
        "inline-flex items-center gap-1 rounded-full border px-2 py-0.5 text-[11px] font-medium leading-none",
        tones[tone],
        className
      )}
      {...props}
    >
      {children}
    </span>
  );
}

export function Stat({
  label,
  value,
  hint,
}: {
  label: string;
  value: ReactNode;
  hint?: string;
}) {
  return (
    <div className="rounded-xl border border-line/60 bg-panel2/50 px-3 py-2.5 transition-colors">
      <div className="text-[11px] uppercase tracking-wide text-ink-dim">{label}</div>
      <div className="mt-1 text-lg font-semibold text-ink">{value}</div>
      {hint && <div className="text-[11px] text-ink-dim">{hint}</div>}
    </div>
  );
}
