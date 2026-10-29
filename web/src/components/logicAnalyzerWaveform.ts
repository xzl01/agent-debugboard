export interface WaveformPoint {
  x: number;
  y: number;
}

/** Keep each pixel bucket's first/last edge, not one aliased sample. Dense
 * activity is compressed, but a mixed bucket can never become a constant level.
 * At most four points per bucket plus the final sample-interval endpoint. */
export function activityWaveformPoints({ samples, channelIndex, start, end, left, width, high, low }: {
  samples: readonly number[];
  channelIndex: number;
  start: number;
  end: number;
  left: number;
  width: number;
  high: number;
  low: number;
}): WaveformPoint[] {
  const span = end - start;
  if (span <= 0) return [];
  const buckets = Math.min(span, Math.max(1, Math.floor(width)));
  const points: WaveformPoint[] = [];
  const bit = (index: number) => (samples[index] >> channelIndex) & 1;
  const push = (index: number) => {
    const point = { x: left + (index - start) / span * width, y: bit(index) ? high : low };
    if (points.at(-1)?.x !== point.x) points.push(point);
  };
  for (let bucket = 0; bucket < buckets; bucket += 1) {
    const first = start + Math.floor(bucket * span / buckets);
    const last = start + Math.floor((bucket + 1) * span / buckets) - 1;
    let firstEdge = -1;
    let lastEdge = -1;
    let previous = bit(first);
    push(first);
    for (let index = first + 1; index <= last; index += 1) {
      const current = bit(index);
      if (current !== previous) {
        if (firstEdge < 0) firstEdge = index;
        lastEdge = index;
      }
      previous = current;
    }
    if (firstEdge >= 0) push(firstEdge);
    if (lastEdge > firstEdge) push(lastEdge);
    push(last);
  }
  points.push({ x: left + width, y: points.at(-1)!.y });
  return points;
}

export function stepWaveformPath(points: readonly WaveformPoint[], yOffset: number): string {
  let d = `M ${points[0]?.x ?? 0} ${points[0] ? yOffset + points[0].y : 0}`;
  for (let pointIndex = 1; pointIndex < points.length; pointIndex += 1) {
    const previous = points[pointIndex - 1];
    const current = points[pointIndex];
    if (!previous || !current) continue;

    if (current.y !== previous.y) {
      d += ` L ${current.x} ${yOffset + previous.y}`;
    }
    d += ` L ${current.x} ${yOffset + current.y}`;
  }
  return d;
}
