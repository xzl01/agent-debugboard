export type SerialLoginState =
  | "waiting"
  | "username_sent"
  | "password_sent"
  | "command_running"
  | "passed"
  | "failed";

export type SerialLoginAction = "send_username" | "send_password" | "run_command" | null;

// A prompt is not guaranteed to remain the final text in the buffer: kernel or
// service logs can arrive immediately after it. Accept a line boundary as well
// as the current end of stream so an otherwise valid prompt is not lost.
const LOGIN_PROMPT = /(?:^|[\r\n])[^\r\n]*(?:login|username):\s*(?=[\r\n]|$)/im;
const PASSWORD_PROMPT = /(?:^|[\r\n])[^\r\n]*password:\s*(?=[\r\n]|$)/im;
const SHELL_PROMPT = /(?:^|[\r\n])[^\r\n]*[#$]\s*(?=[\r\n]|$)/m;

export function stripTerminalControl(text: string) {
  return text
    .replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, "")
    .replaceAll("\uFFFD", "")
    .replace(/[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]/g, "");
}

export function nextSerialLoginAction(
  text: string,
  state: SerialLoginState
): SerialLoginAction {
  const clean = stripTerminalControl(text);
  if (state === "waiting") {
    if (SHELL_PROMPT.test(clean)) return "run_command";
    if (LOGIN_PROMPT.test(clean)) return "send_username";
  }
  if (state === "username_sent") {
    if (SHELL_PROMPT.test(clean)) return "run_command";
    if (PASSWORD_PROMPT.test(clean)) return "send_password";
  }
  if (state === "password_sent" && SHELL_PROMPT.test(clean)) {
    return "run_command";
  }
  return null;
}

function escapeRegExp(value: string) {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

export function commandMarker(runId: number) {
  return `__LINKR_TASK_${runId}__`;
}

export function commandEnvelope(command: string, marker: string, lineEnding = "\r\n") {
  return [
    command.trim(),
    "__linkr_task_status=$?",
    `printf '\\n${marker}:%s\\n' "$__linkr_task_status"`,
    "",
  ].join(lineEnding);
}

export interface SerialCommandCompletion {
  exitCode: number;
  output: string;
}

export function parseCommandCompletion(
  text: string,
  marker: string
): SerialCommandCompletion | null {
  const clean = stripTerminalControl(text);
  const match = new RegExp(`${escapeRegExp(marker)}:(-?\\d+)`).exec(clean);
  if (!match || match.index == null) return null;
  return {
    exitCode: Number(match[1]),
    output: clean.slice(0, match.index).trim(),
  };
}
