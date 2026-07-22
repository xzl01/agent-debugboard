import type { StepAssertion } from "./testScript";

export interface AssertionContext {
  adcValueUa?: number;
  serialOutput?: string;
  exitCode?: number;
  pinDirection?: string;
  pinValue?: number;
  switchRoute?: string;
  peakCurrentUa?: number;
  energyUj?: number;
}

export interface AssertionResult {
  passed: boolean;
  detail: string;
}

export function evaluateAssertion(
  assert: StepAssertion | undefined,
  ctx: AssertionContext,
): AssertionResult {
  if (!assert) return { passed: true, detail: "no assertion" };
  const results: AssertionResult[] = [];
  const requireValue = <T>(value: T | undefined, name: string): value is T => {
    if (value != null) return true;
    results.push({ passed: false, detail: `${name} is unavailable` });
    return false;
  };

  if (assert.current_range != null && requireValue(ctx.adcValueUa, "current")) {
    const minUa = assert.current_range.min_a * 1_000_000;
    const maxUa = assert.current_range.max_a * 1_000_000;
    if (ctx.adcValueUa < minUa || ctx.adcValueUa > maxUa) {
      const actualA = (ctx.adcValueUa / 1_000_000).toFixed(3);
      results.push({
        passed: false,
        detail: `current ${actualA}A outside range [${assert.current_range.min_a}, ${assert.current_range.max_a}]A`,
      });
    } else {
      results.push({ passed: true, detail: `current ${(ctx.adcValueUa / 1_000_000).toFixed(3)}A in range` });
    }
  }

  if (assert.contains != null && requireValue(ctx.serialOutput, "serial output")) {
    const clean = ctx.serialOutput.replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, "").replaceAll("\uFFFD", "");
    if (!clean.includes(assert.contains)) {
      results.push({ passed: false, detail: `output does not contain "${assert.contains}"` });
    } else {
      results.push({ passed: true, detail: `output contains "${assert.contains}"` });
    }
  }

  if (assert.regex != null && requireValue(ctx.serialOutput, "serial output")) {
    const clean = ctx.serialOutput.replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, "").replaceAll("\uFFFD", "");
    try {
      if (assert.regex.length > 500) throw new Error("pattern too long");
      if (!new RegExp(assert.regex).test(clean)) {
        results.push({ passed: false, detail: `output does not match /${assert.regex}/` });
      } else {
        results.push({ passed: true, detail: `output matches /${assert.regex}/` });
      }
    } catch {
      results.push({ passed: false, detail: `invalid regex: ${assert.regex}` });
    }
  }

  if (assert.exit_code != null && requireValue(ctx.exitCode, "exit code")) {
    if (ctx.exitCode !== assert.exit_code) {
      results.push({ passed: false, detail: `exit code ${ctx.exitCode}, expected ${assert.exit_code}` });
    } else {
      results.push({ passed: true, detail: `exit code ${ctx.exitCode}` });
    }
  }

  if (assert.pin_direction != null && requireValue(ctx.pinDirection, "GPIO direction")) {
    if (ctx.pinDirection !== assert.pin_direction) {
      results.push({ passed: false, detail: `direction "${ctx.pinDirection}", expected "${assert.pin_direction}"` });
    } else {
      results.push({ passed: true, detail: `direction "${ctx.pinDirection}"` });
    }
  }

  if (assert.pin_value != null && requireValue(ctx.pinValue, "GPIO value")) {
    if (ctx.pinValue !== assert.pin_value) {
      results.push({ passed: false, detail: `value ${ctx.pinValue}, expected ${assert.pin_value}` });
    } else {
      results.push({ passed: true, detail: `value ${ctx.pinValue}` });
    }
  }

  if (assert.peak_current_max_a != null && requireValue(ctx.peakCurrentUa, "peak current")) {
    const maxUa = assert.peak_current_max_a * 1_000_000;
    if (ctx.peakCurrentUa > maxUa) {
      results.push({
        passed: false,
        detail: `peak ${(ctx.peakCurrentUa / 1_000_000).toFixed(3)}A exceeds max ${assert.peak_current_max_a}A`,
      });
    } else {
      results.push({ passed: true, detail: `peak ${(ctx.peakCurrentUa / 1_000_000).toFixed(3)}A within limit` });
    }
  }

  if (assert.energy_max_j != null && requireValue(ctx.energyUj, "energy")) {
    const maxUj = assert.energy_max_j * 1_000_000;
    if (ctx.energyUj > maxUj) {
      results.push({
        passed: false,
        detail: `energy ${(ctx.energyUj / 1_000_000).toFixed(3)}J exceeds max ${assert.energy_max_j}J`,
      });
    } else {
      results.push({ passed: true, detail: `energy ${(ctx.energyUj / 1_000_000).toFixed(3)}J within limit` });
    }
  }

  if (results.length === 0) return { passed: true, detail: "no assertion" };
  const failures = results.filter((result) => !result.passed);
  return {
    passed: failures.length === 0,
    detail: (failures.length > 0 ? failures : results).map((result) => result.detail).join("; "),
  };
}
