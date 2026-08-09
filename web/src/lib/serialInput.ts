export type SerialLineEnding = "crlf" | "cr" | "lf";

// Interactive serial consoles conventionally submit Enter as a single CR.
// CRLF remains selectable for devices whose command parser explicitly needs it.
export const DEFAULT_SERIAL_LINE_ENDING: SerialLineEnding = "cr";

export const SERIAL_LINE_ENDINGS: Record<SerialLineEnding, string> = {
  crlf: "\r\n",
  cr: "\r",
  lf: "\n",
};

const CONSOLE_PUNCTUATION: Readonly<Record<string, string>> = {
  "。": ".",
  "、": ",",
  "“": "\"",
  "”": "\"",
  "‘": "'",
  "’": "'",
  "《": "<",
  "》": ">",
  "【": "[",
  "】": "]",
  "「": "[",
  "」": "]",
  "『": "[",
  "』": "]",
};

/**
 * Keep terminal commands byte-oriented even when a Chinese IME emits
 * full-width punctuation. Printable full-width ASCII occupies U+FF01-U+FF5E.
 */
export function normalizeConsolePunctuation(input: string): string {
  let output = "";
  for (const character of input) {
    const mapped = CONSOLE_PUNCTUATION[character];
    if (mapped != null) {
      output += mapped;
      continue;
    }
    const codePoint = character.codePointAt(0) ?? 0;
    if (codePoint >= 0xff01 && codePoint <= 0xff5e) {
      output += String.fromCodePoint(codePoint - 0xfee0);
    } else if (codePoint === 0x3000) {
      output += " ";
    } else {
      output += character;
    }
  }
  return output;
}

export function normalizeSerialTerminalInput(
  input: string,
  lineEnding: SerialLineEnding
): string {
  const normalized = normalizeConsolePunctuation(input);
  return normalized.replace(/\r\n|\r|\n/g, SERIAL_LINE_ENDINGS[lineEnding]);
}
