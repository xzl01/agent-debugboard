/** Validate JSON before JSON.parse can erase duplicate object keys at any depth. */
export function assertUniqueJsonObjectKeys(source: string): void {
  let cursor = 0;
  const fail = (detail: string): never => { throw new SyntaxError(`${detail} at JSON offset ${cursor}`); };
  const whitespace = () => { while (/[\x20\t\r\n]/.test(source[cursor] ?? "")) cursor++; };
  const take = (character: string) => {
    whitespace();
    if (source[cursor] !== character) fail(`Expected ${JSON.stringify(character)}`);
    cursor++;
  };
  const string = (): string => {
    take('"');
    let decoded = "";
    while (cursor < source.length) {
      const character = source[cursor++];
      if (character === '"') return decoded;
      if (character.charCodeAt(0) < 0x20) fail("Unescaped string control character");
      if (character !== "\\") { decoded += character; continue; }
      const escaped = source[cursor++];
      if (escaped === "u") {
        const hex = source.slice(cursor, cursor + 4);
        if (!/^[0-9a-fA-F]{4}$/.test(hex)) fail("Invalid Unicode escape");
        decoded += String.fromCharCode(parseInt(hex, 16));
        cursor += 4;
      } else {
        const escapes: Record<string, string> = { '"': '"', "\\": "\\", "/": "/", b: "\b", f: "\f", n: "\n", r: "\r", t: "\t" };
        if (!Object.hasOwn(escapes, escaped)) fail("Invalid string escape");
        decoded += escapes[escaped];
      }
    }
    return fail("Unterminated string");
  };
  const value = (depth: number): void => {
    if (depth > 256) fail("JSON nesting limit exceeded");
    whitespace();
    const character = source[cursor];
    if (character === '"') { string(); return; }
    if (character === "{" || character === "[") {
      cursor++;
      const object = character === "{";
      const end = object ? "}" : "]";
      const keys = new Set<string>();
      whitespace();
      if (source[cursor] === end) { cursor++; return; }
      while (true) {
        if (object) {
          const key = string();
          if (keys.has(key)) fail(`Duplicate JSON key ${JSON.stringify(key)}`);
          keys.add(key);
          take(":");
        }
        value(depth + 1);
        whitespace();
        if (source[cursor] === end) { cursor++; return; }
        take(",");
      }
    }
    const token = /^(?:true|false|null|-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)/.exec(source.slice(cursor));
    if (!token) fail("Expected JSON value");
    cursor += token![0].length;
  };
  value(0);
  whitespace();
  if (cursor !== source.length) fail("Unexpected trailing JSON input");
}
