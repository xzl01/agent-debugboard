import assert from "node:assert/strict";
import test from "node:test";
import { activityWaveformPoints } from "./logicAnalyzerWaveform.ts";
import { resolveStreamWindow } from "./logicAnalyzerStreamWindow.ts";

test("freezes absolute samples before and after the retained ring reaches its capacity", () => {
  const frozen = { start: 4096, end: 8192 };
  const initial = resolveStreamWindow(8192, 8192, 4096, frozen);
  const appended = resolveStreamWindow(12288, 12288, 4096, frozen);
  assert.equal(initial.firstSampleIndex + initial.start, 4096);
  assert.equal(appended.firstSampleIndex + appended.end, 8192);
  assert.equal(appended.historyEvicted, false);
  const shiftedRing = resolveStreamWindow(1001000, 1000000, 4096, frozen);
  assert.equal(shiftedRing.firstSampleIndex + shiftedRing.start, 4096);
  assert.equal(shiftedRing.firstSampleIndex + shiftedRing.end, 8192);
  assert.equal(shiftedRing.historyEvicted, false);
  const evicted = resolveStreamWindow(1010000, 1000000, 4096, frozen);
  assert.equal(evicted.firstSampleIndex + evicted.start, 10000);
  assert.equal(evicted.firstSampleIndex + evicted.end, 14096);
  assert.equal(evicted.historyEvicted, true);
  const following = resolveStreamWindow(1010000, 1000000, 4096, null);
  assert.equal(following.firstSampleIndex + following.end, 1010000);
  assert.equal(following.historyEvicted, false);
});

test("a frozen short initial window does not grow when more samples arrive", () => {
  const window = resolveStreamWindow(8192, 8192, 4096, { start: 0, end: 4 });
  assert.equal(window.span, 4);
  assert.equal(window.start, 0);
  assert.equal(window.end, 4);
});

test("pixel buckets retain alternating activity and single-sample pulses on every selected channel", () => {
  for (const channelIndex of [0, 4]) {
    for (const signal of ["alternating", "positive pulse", "negative pulse"]) {
      const samples = Array.from({ length: 4096 }, (_, index) =>
        (signal === "alternating" ? index % 2 : signal === "positive pulse" ? Number(index === 7) : Number(index !== 7)) << channelIndex);
      const points = activityWaveformPoints({ samples, channelIndex, start: 0, end: samples.length,
        left: 48, width: 740, high: 4, low: 24 });
      assert.deepEqual(new Set(points.map((point) => point.y)), new Set([4, 24]));
      assert.ok(points.length <= 740 * 4 + 1);
      assert.equal(points[0].x, 48);
      assert.equal(points.at(-1)!.x, 788);
      assert.ok(points.every((point, index) => index === 0 || point.x > points[index - 1].x));
      if (signal !== "alternating") {
        assert.ok(points.some((point) => point.x === 48 + 7 / 4096 * 740));
        assert.ok(points.some((point) => point.x === 48 + 8 / 4096 * 740));
      }
    }
  }
});

test("constant signals remain constant and the final sample interval reaches the plot boundary", () => {
  for (const value of [0, 1]) {
    const points = activityWaveformPoints({ samples: [value, value, value], channelIndex: 0,
      start: 1, end: 3, left: 48, width: 740, high: 4, low: 24 });
    assert.deepEqual(points, [{ x: 48, y: value ? 4 : 24 }, { x: 418, y: value ? 4 : 24 },
      { x: 788, y: value ? 4 : 24 }]);
  }
});
