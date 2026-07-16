import assert from "node:assert/strict";
import test from "node:test";
import {
  commandEnvelope,
  commandMarker,
  nextSerialLoginAction,
  parseCommandCompletion,
  stripTerminalControl,
} from "./serialTask.ts";

test("recognizes login, password, and shell prompts in order", () => {
  assert.equal(nextSerialLoginAction("radxa login: ", "waiting"), "send_username");
  assert.equal(nextSerialLoginAction("Password: ", "username_sent"), "send_password");
  assert.equal(nextSerialLoginAction("radxa@target:~$ ", "password_sent"), "run_command");
});

test("uses an existing shell without sending credentials", () => {
  assert.equal(nextSerialLoginAction("root@target:~# ", "waiting"), "run_command");
});

test("recognizes a prompt followed by asynchronous system logs", () => {
  assert.equal(
    nextSerialLoginAction("radxa-cubie-a7a login: \r\n[  12.3] background message\r\n", "waiting"),
    "send_username"
  );
  assert.equal(
    nextSerialLoginAction("radxa@target:~$ \r\n[  13.0] service notice\r\n", "password_sent"),
    "run_command"
  );
});

test("wraps a command and parses its exit marker", () => {
  const marker = commandMarker(42);
  const envelope = commandEnvelope("uname -a", marker);
  assert.match(envelope, /uname -a/);
  assert.match(envelope, /__LINKR_TASK_42__/);

  assert.deepEqual(
    parseCommandCompletion(`Linux target 6.1\r\n${marker}:0\r\n`, marker),
    { exitCode: 0, output: "Linux target 6.1" }
  );
});

test("does not complete before the unique marker arrives", () => {
  assert.equal(parseCommandCompletion("still running", commandMarker(9)), null);
});

test("ignores NUL-only UART traffic", () => {
  assert.equal(stripTerminalControl("\0".repeat(128)), "");
  assert.equal(stripTerminalControl("\uFFFD\x01\x7f"), "");
  assert.equal(stripTerminalControl("\x1b[32mready\x1b[0m\0"), "ready");
});
