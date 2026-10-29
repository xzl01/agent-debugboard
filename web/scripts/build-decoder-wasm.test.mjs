import assert from "node:assert/strict";
import { chmod, mkdtemp, mkdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { decoderWasmBindgenArgs, resolveRustCommand } from "./build-decoder-wasm.mjs";

test("prefers the Nix PATH tool over a stale rustup home shim", async (t) => {
  const root = await mkdtemp(path.join(tmpdir(), "decoder-command-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const bin = path.join(root, "bin");
  const home = path.join(root, "home");
  const rustupBin = path.join(home, ".cargo", "bin");
  await mkdir(bin, { recursive: true });
  await mkdir(rustupBin, { recursive: true });
  await writeFile(path.join(bin, "wasm-bindgen"), "#!/bin/sh\nexit 0\n");
  await chmod(path.join(bin, "wasm-bindgen"), 0o755);
  await writeFile(path.join(rustupBin, "wasm-bindgen"), "stale shim\n");

  assert.equal(resolveRustCommand("wasm-bindgen", {
    home,
    platform: "linux",
    pathValue: bin,
  }), "wasm-bindgen");
});

test("wasm-bindgen args remove the name section and keep web output options", () => {
  assert.deepEqual(
    decoderWasmBindgenArgs({
      wasm: "radxa_logic_decoder.wasm",
      outDir: "assets/decoder",
      outName: "logic-decoder",
    }),
    [
      "radxa_logic_decoder.wasm",
      "--target",
      "web",
      "--out-dir",
      "assets/decoder",
      "--out-name",
      "logic-decoder",
      "--no-typescript",
      "--remove-name-section",
    ]
  );
});
