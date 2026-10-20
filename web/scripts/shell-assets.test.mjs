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
});

test("the lower-right arm uses the power-rail style instead of UART activity", async () => {
  const css = await readFile(new URL("../src/index.css", import.meta.url), "utf8");
  const activeRule = css.match(/\[data-indicator="vdd_5v"\]\[data-active="true"\]\s*\{([^}]+)\}/)?.[1];

  assert.ok(activeRule, "the vdd_5v on-state rule must exist");
  assert.match(activeRule, /color:\s*rgb\(var\(--c-identity\)\);/);
  assert.match(activeRule, /opacity:\s*1;/);
  assert.doesNotMatch(css, /uart_bridge|linkr-logo-uart-heartbeat/);
});

test("logo power feedback stays in flow and wraps within the header instead of covering navigation", async () => {
  const css = await readFile(new URL("../src/index.css", import.meta.url), "utf8");
  const logo = await readFile(new URL("../src/components/LinkrLogo.tsx", import.meta.url), "utf8");
  const bounds = css.match(/\.header-power-status\s*\{([^}]+)\}/)?.[1];

  assert.ok(bounds, "power feedback must have its own bounded layout slot");
  assert.match(bounds, /position:\s*static;/);
  assert.match(bounds, /flex:\s*0\s+0\s+auto;/);
  assert.match(bounds, /min-width:\s*0;/);
  assert.match(bounds, /max-width:\s*100%;/);
  assert.match(bounds, /white-space:\s*normal;/);
  assert.match(bounds, /overflow-wrap:\s*anywhere;/);
  assert.match(bounds, /pointer-events:\s*none;/);
  assert.doesNotMatch(logo, /top-full|group-hover:opacity|group-focus-visible:opacity|\btitle=/);
  const emphasis = css.match(/header:has\(\[data-testid="logo-vdd5v-toggle"\]:hover\) \.header-power-status,\s*header:has\(\[data-testid="logo-vdd5v-toggle"\]:focus-visible\) \.header-power-status\s*\{([^}]+)\}/)?.[1];
  assert.ok(emphasis, "pointer and keyboard feedback must share the same reserved slot");
  assert.match(emphasis, /border-color:/);
  assert.match(emphasis, /background-color:/);
  assert.doesNotMatch(emphasis, /position|inset|top|left|width|height|transform|margin|padding|display|opacity/);
});
