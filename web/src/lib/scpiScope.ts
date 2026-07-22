import type { LogicAnalyzerTriggerType } from "./types";

export const RIGOL_PINS = [10, 16, 11, 17, 12, 18, 13, 19, 14, 20, 15, 29, 7, 8, 9] as const;
export const RIGOL_LIVE_SAMPLES = 600;
export const RIGOL_TRIGGER_SAMPLE = RIGOL_LIVE_SAMPLES / 2;
export const RIGOL_HORIZONTAL_DIVS = 12;

export type ScpiEvent =
  | { type: "line"; text: string }
  | { type: "block"; payload: Uint8Array };

export class ScpiStreamReader {
  private buffer = new Uint8Array(0);

  feed(chunk: Uint8Array): ScpiEvent[] {
    const merged = new Uint8Array(this.buffer.length + chunk.length);
    merged.set(this.buffer);
    merged.set(chunk, this.buffer.length);
    this.buffer = merged;

    const events: ScpiEvent[] = [];
    for (;;) {
      if (this.buffer.length === 0) break;
      if (this.buffer[0] === 0x23) {
        if (this.buffer.length < 2) break;
        const ndigits = this.buffer[1] - 0x30;
        if (ndigits < 1 || ndigits > 9) {
          this.buffer = this.buffer.slice(1);
          continue;
        }
        if (this.buffer.length < 2 + ndigits) break;
        let len = 0;
        for (let i = 0; i < ndigits; i++) {
          const d = this.buffer[2 + i] - 0x30;
          if (d < 0 || d > 9) {
            len = -1;
            break;
          }
          len = len * 10 + d;
        }
        if (len < 0) {
          this.buffer = this.buffer.slice(1);
          continue;
        }
        if (this.buffer.length < 2 + ndigits + len) break;
        events.push({
          type: "block",
          payload: this.buffer.slice(2 + ndigits, 2 + ndigits + len),
        });
        this.buffer = this.buffer.slice(2 + ndigits + len);
        continue;
      }
      const nl = this.buffer.indexOf(0x0a);
      if (nl < 0) {
        if (this.buffer.length > 4096) {
          this.buffer = new Uint8Array(0);
        }
        break;
      }
      const text = new TextDecoder()
        .decode(this.buffer.slice(0, nl))
        .replace(/\r$/, "");
      this.buffer = this.buffer.slice(nl + 1);
      events.push({ type: "line", text });
    }
    return events;
  }
}

export function timebaseForRate(rateHz: number): number {
  return RIGOL_LIVE_SAMPLES / (RIGOL_HORIZONTAL_DIVS * rateHz);
}

export function formatScpiNumber(value: number): string {
  return value.toExponential(7);
}

export function rigolSlopeForTrigger(trigger: LogicAnalyzerTriggerType): "POS" | "NEG" {
  return trigger === "rising" ? "POS" : "NEG";
}

export function rigolSourceIndexForPin(pin: number): number {
  const idx = (RIGOL_PINS as readonly number[]).indexOf(pin);
  return idx >= 0 ? idx : 0;
}

export function repackFrameBits(frame: ArrayLike<number>, outPins: number[]): number[] {
  const map = outPins.map((pin) => rigolSourceIndexForPin(pin));
  const out = new Array<number>(frame.length);
  for (let i = 0; i < frame.length; i++) {
    const v = frame[i];
    let packed = 0;
    for (let bit = 0; bit < map.length; bit++) {
      if ((v & (1 << map[bit])) !== 0) {
        packed |= 1 << bit;
      }
    }
    out[i] = packed;
  }
  return out;
}
