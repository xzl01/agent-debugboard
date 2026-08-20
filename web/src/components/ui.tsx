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
  headerClassName,
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
  headerClassName?: string;
  contentClassName?: string;
}) {
  return (
    <section
      className={cn(
        "flex min-h-0 flex-col rounded-2xl border border-line/80 bg-panel transition-colors duration-200",
        className
      )}
    >
      {(title || right || headerLeading) && (
        <header className={cn(
          "flex flex-wrap items-center justify-between gap-3 border-b border-line/60 px-4 py-3",
          headerClassName
        )}>
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
                  <h2 className="truncate text-sm font-semibold text-ink">
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

export function WorkspaceModeHeader({
  icon: Icon,
  title,
  subtitle,
  status,
  children,
}: {
  readonly icon: LucideIcon;
  readonly title: string;
  readonly subtitle: string;
  readonly status?: ReactNode;
  readonly children: ReactNode;
}) {
  return (
    <header className="grid gap-3 border-b border-line/60 px-3 py-3 sm:px-4 lg:grid-cols-[minmax(220px,280px)_minmax(0,760px)] lg:items-center">
      <div className="flex min-w-0 items-center gap-3">
        <span className="grid h-9 w-9 shrink-0 place-items-center rounded-xl bg-brand/10 text-brand">
          <Icon size={17} />
        </span>
        <div className="min-w-0 flex-1">
          <div className="flex min-w-0 items-center gap-2">
            <h2 className="truncate text-sm font-semibold tracking-[-0.01em] text-ink">{title}</h2>
            {status && <span className="shrink-0">{status}</span>}
          </div>
          <p className="mt-0.5 truncate text-[11px] leading-4 text-ink-dim">{subtitle}</p>
        </div>
      </div>
      {children}
    </header>
  );
}

export function WorkspaceModeTab({
  selected,
  icon: Icon,
  label,
  summary,
  className,
  ...props
}: ButtonHTMLAttributes<HTMLButtonElement> & {
  readonly selected: boolean;
  readonly icon: LucideIcon;
  readonly label: string;
  readonly summary: string;
}) {
  return (
    <button
      type="button"
      className={cn(
        "flex min-h-12 min-w-0 items-center gap-2 rounded-lg border px-3 text-left transition-[color,background-color,border-color,box-shadow] duration-150 disabled:cursor-not-allowed disabled:opacity-50",
        selected
          ? "border-brand/25 bg-panel text-ink ring-1 ring-inset ring-brand/10"
          : "border-transparent text-ink-dim hover:border-line/70 hover:bg-panel/70 hover:text-ink",
        className,
      )}
      {...props}
    >
      <span
        className={cn(
          "grid h-7 w-7 shrink-0 place-items-center rounded-md transition-colors duration-150",
          selected ? "bg-brand text-on-brand" : "bg-panel text-ink-dim",
        )}
      >
        <Icon size={14} />
      </span>
      <span className="min-w-0">
        <span className="block truncate text-xs font-semibold">{label}</span>
        <span className="hidden truncate text-[10px] font-normal text-ink-dim sm:block">{summary}</span>
      </span>
    </button>
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
      "border border-line/80 bg-panel2/80 text-ink hover:border-line hover:bg-panel2",
    primary:
      "border border-transparent bg-brand text-on-brand hover:bg-brand/90",
    danger:
      "border border-transparent bg-danger text-on-danger hover:bg-danger/90",
    ghost:
      "bg-transparent text-ink-dim hover:bg-panel2 hover:text-ink border border-transparent",
  };
  return (
    <button
      ref={ref}
      className={cn(
        "inline-flex min-h-10 items-center justify-center gap-2 rounded-xl px-3 py-2 text-sm font-medium transition-[color,background-color,border-color,transform] duration-150 active:scale-[0.99] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40 disabled:cursor-not-allowed disabled:opacity-50 disabled:active:scale-100",
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
    <div className="rounded-xl bg-panel2/70 px-3 py-2.5 transition-colors">
      <div className="text-[11px] uppercase tracking-wide text-ink-dim">{label}</div>
      <div className="mt-1 text-lg font-semibold text-ink">{value}</div>
      {hint && <div className="text-[11px] text-ink-dim">{hint}</div>}
    </div>
  );
}
