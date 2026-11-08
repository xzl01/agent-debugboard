import assert from "node:assert/strict";
import { chmod, mkdtemp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import {
  decoderWasmBindgenArgs,
  decoderWasmCargoArgs,
  decoderWasmProfile,
  decoderWasmRawPath,
  resolveRustCommand,
} from "./build-decoder-wasm.mjs";

test("WASM cargo args select the dedicated profile and override crate types in Cargo", () => {
  assert.deepEqual(decoderWasmCargoArgs({ manifest: "decoder/Cargo.toml" }), [
    "rustc",
    "--manifest-path",
    "decoder/Cargo.toml",
    "--profile",
    "wasm-release",
    "--target",
    "wasm32-unknown-unknown",
    "--features",
    "wasm",
    "--lib",
    "--crate-type",
    "cdylib",
  ]);
});

test("raw WASM stays below decoder target in the dedicated profile directory", () => {
  assert.equal(
    decoderWasmRawPath("decoder"),
    path.join("decoder", "target", "wasm32-unknown-unknown", "wasm-release", "radxa_logic_decoder.wasm")
  );
});

test("the WASM profile enables fat LTO without changing native release or library types", async () => {
  const manifest = await readFile(new URL("../decoder/Cargo.toml", import.meta.url), "utf8");
  assert.equal(decoderWasmProfile, "wasm-release");
  const profile = manifest.split(/(?=^\[)/m).find((section) => section.startsWith(`[profile.${decoderWasmProfile}]`));
  assert.ok(profile, "dedicated WASM profile must exist");
  assert.deepEqual(profile.trim().split(/\r?\n/).slice(1), [
    'inherits = "release"',
    "opt-level = 3",
    'lto = "fat"',
    "codegen-units = 1",
  ]);
  assert.doesNotMatch(manifest, /^\[profile\.release(?:\.|\])/m);
  assert.match(manifest, /^\[lib\]\r?\ncrate-type = \["cdylib", "rlib"\]/m);
});

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
