import assert from "node:assert/strict";
import { chmod, mkdtemp, mkdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { resolveRustCommand } from "./build-decoder-wasm.mjs";

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
