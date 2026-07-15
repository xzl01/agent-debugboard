import { readdir, readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const webRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const distRoot = process.env.VITE_OUT_DIR
  ? path.resolve(process.env.VITE_OUT_DIR)
  : path.join(webRoot, "dist");
const expectedFiles = ["assets/app.css", "assets/app.js", "index.html"];

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

const actualFiles = (await listFiles(distRoot)).sort();
if (JSON.stringify(actualFiles) !== JSON.stringify(expectedFiles)) {
  throw new Error(
    `Firmware Web build must contain exactly ${expectedFiles.join(", ")}; got ${actualFiles.join(", ")}`
  );
}

const indexHtml = await readFile(path.join(distRoot, "index.html"), "utf8");
for (const asset of ["/assets/app.css", "/assets/app.js"]) {
  if (!indexHtml.includes(asset)) {
    throw new Error(`Firmware Web build does not reference ${asset}`);
  }
}
