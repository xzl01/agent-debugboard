import { readdir, readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const webRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const distRoot = process.env.VITE_OUT_DIR
  ? path.resolve(process.env.VITE_OUT_DIR)
  : path.join(webRoot, "dist");
const expectedFiles = [
  "assets/app.css",
  "assets/app.js",
  "assets/decoder/logic-decoder.js",
  "assets/decoder/logic-decoder_bg.wasm",
  "index.html",
];

async function listFiles(directory, prefix = "") {
  const entries = await readdir(directory, { withFileTypes: true });
  const files = [];

  for (const entry of entries) {
    const relativePath = path.posix.join(prefix, entry.name);
    if (entry.isDirectory()) {
      files.push(...(await listFiles(path.join(directory, entry.name), relativePath)));
    } else {
      files.push(relativePath);
    }
  }

  return files;
}

export async function verifyFirmwareBuild(root = distRoot) {
  const actualFiles = (await listFiles(root)).sort();
  if (JSON.stringify(actualFiles) !== JSON.stringify(expectedFiles)) {
    throw new Error(
      `Firmware Web build must contain exactly ${expectedFiles.join(", ")}; got ${actualFiles.join(", ")}`
    );
  }

  const indexHtml = await readFile(path.join(root, "index.html"), "utf8");
  for (const asset of ["/assets/app.css", "/assets/app.js"]) {
    if (!indexHtml.includes(asset)) {
      throw new Error(`Firmware Web build does not reference ${asset}`);
    }
  }

  const decoderJs = await readFile(path.join(root, "assets/decoder/logic-decoder.js"), "utf8");
  if (!decoderJs.includes("logic-decoder_bg.wasm")) {
    throw new Error("Decoder JS glue must load /assets/decoder/logic-decoder_bg.wasm relative to itself");
  }
}

if (import.meta.url === `file://${process.argv[1]}`) {
  await verifyFirmwareBuild();
}
