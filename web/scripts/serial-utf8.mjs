import { StringDecoder } from "node:string_decoder";

export function createSerialUtf8Decoder() {
  const decoder = new StringDecoder("utf8");
  return {
    decode(chunk) {
      return decoder.write(chunk);
    },
    flush() {
      return decoder.end();
    },
  };
}
