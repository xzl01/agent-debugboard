export type SerialConnectionFailureStage = "request" | "open" | "bridge";

export const LINUX_SERIAL_PERMISSION_COMMAND =
  'sudo usermod -aG dialout "$(id -un)"';

interface NavigatorPlatformInfo {
  platform?: string;
  userAgent?: string;
  userAgentData?: { platform?: string };
}

export function isLinuxSerialHost(info: NavigatorPlatformInfo): boolean {
  return [info.userAgentData?.platform, info.platform, info.userAgent]
    .filter((value): value is string => typeof value === "string")
    .some((value) => /linux/i.test(value));
}

export function shouldShowLinuxSerialPermissionHelp({
  isLinux,
  stage,
  error,
}: {
  isLinux: boolean;
  stage: SerialConnectionFailureStage;
  error: unknown;
}): boolean {
  if (!isLinux || stage === "request") return false;

  const name = error instanceof Error ? error.name : "";
  const message = error instanceof Error ? error.message : String(error ?? "");
  const permissionFailure = /\b(?:EACCES|EPERM)\b|permission denied|access denied|operation not permitted/i
    .test(`${name} ${message}`);
  if (permissionFailure) return true;

  return stage === "open" && (
    name === "NetworkError" ||
    /(?:failed|unable|cannot) to open (?:the )?serial port/i.test(message)
  );
}
