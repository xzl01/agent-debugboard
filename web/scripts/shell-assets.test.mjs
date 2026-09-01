import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

test("index provides a meaningful meta description", async () => {
  const html = await readFile(new URL("../index.html", import.meta.url), "utf8");
  const description = html.match(/<meta\s+name="description"\s+content="([^"]+)"\s*\/?>/i)?.[1];

  assert.ok(description, "index.html must contain a meta description");
  assert.ok(description.length >= 50, "meta description must summarize the dashboard");
});

test("public robots file permits the production dashboard", async () => {
  const robots = await readFile(new URL("../public/robots.txt", import.meta.url), "utf8");

  assert.match(robots, /^User-agent:\s*\*$/m);
  assert.match(robots, /^Allow:\s*\/$/m);
  assert.doesNotMatch(robots, /<!doctype html>/i);
});

test("logic rotor rotates around the painted X bounds", async () => {
  const css = await readFile(new URL("../src/index.css", import.meta.url), "utf8");
  const rule = css.match(
    /\[data-logic-analyzer-active="true"\]\s*\n\s*\[data-linkr-rotor\]\s*\{([^}]+)\}/,
  )?.[1];

  assert.ok(rule, "logic-active rotor rule must exist");
  assert.match(rule, /transform-box:\s*fill-box;/);
  assert.match(rule, /transform-origin:\s*center;/);
});

test("ready center heartbeat uses the faster independent cadence", async () => {
  const css = await readFile(new URL("../src/index.css", import.meta.url), "utf8");

  assert.match(css, /animation:\s*linkr-logo-heartbeat\s+800ms\s+ease-in-out\s+infinite;/);
  assert.match(css, /animation:\s*linkr-logo-uart-heartbeat\s+800ms\s+ease-in-out\s+infinite;/);
});
