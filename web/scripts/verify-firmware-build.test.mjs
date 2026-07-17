import test from "node:test";
import assert from "node:assert/strict";
import { mkdtemp, mkdir, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { verifyFirmwareBuild } from "./verify-firmware-build.mjs";

async function withFirmwareDist(files, fn) {
  const root = await mkdtemp(path.join(os.tmpdir(), "linkr-web-dist-"));
  try {
    for (const [relativePath, content] of Object.entries(files)) {
      const fullPath = path.join(root, relativePath);
      await mkdir(path.dirname(fullPath), { recursive: true });
      await writeFile(fullPath, content);
    }
    await fn(root);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
}

const validFiles = {
  "index.html": '<link rel="stylesheet" href="/assets/app.css"><script type="module" src="/assets/app.js"></script>',
  "assets/app.css": "body{}",
  "assets/app.js": "console.log('app')",
  "assets/decoder/logic-decoder.js": "new URL('logic-decoder_bg.wasm', import.meta.url)",
  "assets/decoder/logic-decoder_bg.wasm": "\0asm",
};

test("firmware verification accepts the fixed app and decoder asset set", async () => {
  await withFirmwareDist(validFiles, async (root) => {
    await verifyFirmwareBuild(root);
  });
});

test("firmware verification rejects missing decoder wasm", async () => {
  const files = { ...validFiles };
  delete files["assets/decoder/logic-decoder_bg.wasm"];

  await withFirmwareDist(files, async (root) => {
    await assert.rejects(
      verifyFirmwareBuild(root),
      /Firmware Web build must contain exactly/
    );
  });
});

test("firmware verification rejects decoder glue without wasm URL", async () => {
  await withFirmwareDist(
    { ...validFiles, "assets/decoder/logic-decoder.js": "export default function init() {}" },
    async (root) => {
      await assert.rejects(verifyFirmwareBuild(root), /Decoder JS glue must load/);
    }
  );
});
