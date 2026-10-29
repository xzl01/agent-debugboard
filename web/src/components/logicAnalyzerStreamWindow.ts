export interface FrozenSampleWindow {
  start: number;
  end: number;
}

/** Absolute sample positions survive incoming data and ring-buffer eviction. */
export function resolveStreamWindow(total: number, retained: number, span: number, frozen: FrozenSampleWindow | null) {
  const firstSampleIndex = total - retained;
  const length = Math.min(retained, frozen ? frozen.end - frozen.start : span);
  const absoluteStart = frozen
    ? Math.min(total - length, Math.max(firstSampleIndex, frozen.start))
    : total - length;
  const start = absoluteStart - firstSampleIndex;
  return {
    len: retained, span: length, start, end: start + length, firstSampleIndex,
    historyEvicted: frozen != null && frozen.start < firstSampleIndex,
  };
}
