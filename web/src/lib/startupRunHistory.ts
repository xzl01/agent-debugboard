export interface StartupRunHistoryEntry {
  rail: string;
}

const DEFAULT_PER_RAIL_LIMIT = 2;

export function appendStartupRunHistory<TRun extends StartupRunHistoryEntry>(
  previous: readonly TRun[],
  completedRun: TRun,
  perRailLimit = DEFAULT_PER_RAIL_LIMIT,
): TRun[] {
  const counts = new Map<string, number>();
  const retained: TRun[] = [];

  for (const run of [...previous, completedRun].reverse()) {
    const count = counts.get(run.rail) ?? 0;
    if (count >= perRailLimit) continue;
    counts.set(run.rail, count + 1);
    retained.push(run);
  }

  return retained.reverse();
}
