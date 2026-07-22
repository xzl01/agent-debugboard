import { access, mkdir, rm } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const webRoot = path.resolve(scriptDir, "..");
const decoderRoot = path.join(webRoot, "decoder");
const decoderManifest = path.join(decoderRoot, "Cargo.toml");
const distRoot = process.env.VITE_OUT_DIR ? path.resolve(process.env.VITE_OUT_DIR) : path.join(webRoot, "dist");
const decoderOutDir = path.join(distRoot, "assets", "decoder");
const wasmTargetDir = path.join(decoderRoot, "target", "wasm32-unknown-unknown", "release");
const rawWasm = path.join(wasmTargetDir, "radxa_logic_decoder.wasm");
const outName = "logic-decoder";
const publicJsPath = "/assets/decoder/logic-decoder.js";
const publicWasmPath = "/assets/decoder/logic-decoder_bg.wasm";

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: options.cwd ?? webRoot,
    env: process.env,
    stdio: "inherit",
  });

  if (result.error?.code === "ENOENT") {
    throw new Error(`${command} is required to build the browser decoder WASM asset`);
  }
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0) {
    throw new Error(`${command} ${args.join(" ")} failed with exit code ${result.status}`);
  }
}

function capture(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: options.cwd ?? webRoot,
    env: process.env,
    encoding: "utf8",
  });

  if (result.error?.code === "ENOENT") {
    throw new Error(`${command} is required to build the browser decoder WASM asset`);
  }
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0) {
    throw new Error(`${command} ${args.join(" ")} failed with exit code ${result.status}`);
  }
  return result.stdout.trim();
}

function tryCapture(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: options.cwd ?? webRoot,
    env: process.env,
    encoding: "utf8",
  });

  if (result.error || result.status !== 0) {
    return null;
  }
  return result.stdout.trim();
}

async function ensureFile(file, message) {
  try {
    await access(file);
  } catch {
    throw new Error(message);
  }
}

function ensureWasmTargetInstalled() {
  const rustupTargets = tryCapture("rustup", ["target", "list", "--installed"]);
  if (rustupTargets?.split(/\r?\n/).includes("wasm32-unknown-unknown")) {
    return;
  }

  const rustcTargets = capture("rustc", ["--print", "target-list"]);
  if (!rustcTargets.split(/\r?\n/).includes("wasm32-unknown-unknown")) {
    throw new Error(
      "Rust target wasm32-unknown-unknown is required; install it with `rustup target add wasm32-unknown-unknown`"
    );
  }
}

export async function buildDecoderWasm() {
  await ensureFile(decoderManifest, `Decoder manifest not found at ${decoderManifest}`);
  run("cargo", [
    "build",
    "--manifest-path",
    decoderManifest,
    "--release",
    "--target",
    "wasm32-unknown-unknown",
    "--features",
    "wasm",
  ]);
  await ensureFile(rawWasm, `Cargo did not produce expected decoder WASM at ${rawWasm}`);

  await rm(decoderOutDir, { recursive: true, force: true });
  await mkdir(decoderOutDir, { recursive: true });
  run("wasm-bindgen", [
    rawWasm,
    "--target",
    "web",
    "--out-dir",
    decoderOutDir,
    "--out-name",
    outName,
    "--no-typescript",
  ]);

  await ensureFile(path.join(decoderOutDir, `${outName}.js`), `wasm-bindgen did not produce ${publicJsPath}`);
  await ensureFile(path.join(decoderOutDir, `${outName}_bg.wasm`), `wasm-bindgen did not produce ${publicWasmPath}`);
}

if (import.meta.url === `file://${process.argv[1]}`) {
  try {
    ensureWasmTargetInstalled();
    await buildDecoderWasm();
  } catch (error) {
    console.error(error instanceof Error ? error.message : error);
    process.exit(1);
  }
}
