import { describe, it } from "node:test";
import assert from "node:assert/strict";
import { evaluateAssertion } from "./testAssertions.ts";

describe("evaluateAssertion", () => {
  it("passes when no assertion is defined", () => {
    const result = evaluateAssertion(undefined, {});
    assert.equal(result.passed, true);
    assert.equal(result.detail, "no assertion");
  });

  it("fails closed when assertion context is unavailable", () => {
    const result = evaluateAssertion({ contains: "hello" }, {});
    assert.equal(result.passed, false);
    assert.equal(result.detail, "serial output is unavailable");
  });

  it("evaluates every configured assertion", () => {
    const result = evaluateAssertion(
      { contains: "Linux", exit_code: 0 },
      { serialOutput: "Linux version 6.1", exitCode: 1 },
    );
    assert.equal(result.passed, false);
    assert.match(result.detail, /exit code 1/);
  });

  describe("current_range", () => {
    it("passes when current is within range", () => {
      const result = evaluateAssertion(
        { current_range: { min_a: 0, max_a: 3 } },
        { adcValueUa: 1_500_000 },
      );
      assert.equal(result.passed, true);
      assert.match(result.detail, /1\.500A in range/);
    });

    it("fails when current is below range", () => {
      const result = evaluateAssertion(
        { current_range: { min_a: 1, max_a: 3 } },
        { adcValueUa: 500_000 },
      );
      assert.equal(result.passed, false);
      assert.match(result.detail, /0\.500A outside range/);
    });

    it("fails when current is above range", () => {
      const result = evaluateAssertion(
        { current_range: { min_a: 0, max_a: 2 } },
        { adcValueUa: 2_500_000 },
      );
      assert.equal(result.passed, false);
      assert.match(result.detail, /2\.500A outside range/);
    });

    it("passes at exact boundary values", () => {
      const min = evaluateAssertion(
        { current_range: { min_a: 1, max_a: 3 } },
        { adcValueUa: 1_000_000 },
      );
      assert.equal(min.passed, true);

      const max = evaluateAssertion(
        { current_range: { min_a: 1, max_a: 3 } },
        { adcValueUa: 3_000_000 },
      );
      assert.equal(max.passed, true);
    });
  });

  describe("contains", () => {
    it("passes when output contains string", () => {
      const result = evaluateAssertion(
        { contains: "Linux" },
        { serialOutput: "Linux version 6.1.0" },
      );
      assert.equal(result.passed, true);
      assert.match(result.detail, /contains "Linux"/);
    });

    it("fails when output does not contain string", () => {
      const result = evaluateAssertion(
        { contains: "Darwin" },
        { serialOutput: "Linux version 6.1.0" },
      );
      assert.equal(result.passed, false);
    });

    it("strips ANSI escape codes before matching", () => {
      const result = evaluateAssertion(
        { contains: "hello" },
        { serialOutput: "\x1b[32mhello\x1b[0m world" },
      );
      assert.equal(result.passed, true);
    });
  });

  describe("regex", () => {
    it("passes when output matches regex", () => {
      const result = evaluateAssertion(
        { regex: "Linux \\d+\\.\\d+" },
        { serialOutput: "Linux 6.1.0" },
      );
      assert.equal(result.passed, true);
    });

    it("fails when output does not match regex", () => {
      const result = evaluateAssertion(
        { regex: "^Darwin" },
        { serialOutput: "Linux 6.1.0" },
      );
      assert.equal(result.passed, false);
    });

    it("fails gracefully on invalid regex", () => {
      const result = evaluateAssertion(
        { regex: "[unclosed" },
        { serialOutput: "hello" },
      );
      assert.equal(result.passed, false);
      assert.match(result.detail, /invalid regex/);
    });
  });

  describe("exit_code", () => {
    it("passes when exit code matches", () => {
      const result = evaluateAssertion(
        { exit_code: 0 },
        { exitCode: 0 },
      );
      assert.equal(result.passed, true);
    });

    it("fails when exit code differs", () => {
      const result = evaluateAssertion(
        { exit_code: 0 },
        { exitCode: 1 },
      );
      assert.equal(result.passed, false);
      assert.match(result.detail, /exit code 1, expected 0/);
    });
  });

  describe("pin_direction", () => {
    it("passes when direction matches", () => {
      const result = evaluateAssertion(
        { pin_direction: "output" },
        { pinDirection: "output" },
      );
      assert.equal(result.passed, true);
    });

    it("fails when direction differs", () => {
      const result = evaluateAssertion(
        { pin_direction: "input" },
        { pinDirection: "output" },
      );
      assert.equal(result.passed, false);
    });
  });

  describe("pin_value", () => {
    it("passes when value matches", () => {
      const result = evaluateAssertion(
        { pin_value: 1 },
        { pinValue: 1 },
      );
      assert.equal(result.passed, true);
    });

    it("fails when value differs", () => {
      const result = evaluateAssertion(
        { pin_value: 0 },
        { pinValue: 1 },
      );
      assert.equal(result.passed, false);
    });
  });

  describe("peak_current_max_a", () => {
    it("passes when peak is within limit", () => {
      const result = evaluateAssertion(
        { peak_current_max_a: 5 },
        { peakCurrentUa: 3_000_000 },
      );
      assert.equal(result.passed, true);
      assert.match(result.detail, /within limit/);
    });

    it("fails when peak exceeds limit", () => {
      const result = evaluateAssertion(
        { peak_current_max_a: 2 },
        { peakCurrentUa: 3_000_000 },
      );
      assert.equal(result.passed, false);
      assert.match(result.detail, /exceeds max/);
    });
  });

  describe("energy_max_j", () => {
    it("passes when energy is within limit", () => {
      const result = evaluateAssertion(
        { energy_max_j: 10 },
        { energyUj: 5_000_000 },
      );
      assert.equal(result.passed, true);
    });

    it("fails when energy exceeds limit", () => {
      const result = evaluateAssertion(
        { energy_max_j: 1 },
        { energyUj: 2_000_000 },
      );
      assert.equal(result.passed, false);
    });
  });
});
