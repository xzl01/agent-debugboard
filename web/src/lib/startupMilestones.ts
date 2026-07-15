export type BootloaderMode = "auto" | "uboot" | "uefi";
export type DetectedBootloader = Exclude<BootloaderMode, "auto">;

export interface StartupDetection {
  bootrom: boolean;
  bootloader?: DetectedBootloader;
  kernel: boolean;
  login: boolean;
}

const BOOT_ROM_PATTERN = /(?:HELLO!\s*)?BOOT0(?:\s+is\s+starting)?|BootROM|Boot ROM|ROM boot/i;
const UBOOT_PATTERN = /U-Boot(?:\s+SPL)?\s|Hit any key to stop autoboot|Autoboot in\s+\d/i;
const UEFI_PATTERN = /UEFI (?:Start|firmware|Interactive Shell)|EDK\s*II|EDK2|TianoCore|Boot Manager|Press ESC.*startup\.nsh/i;
const KERNEL_PATTERN = /Starting kernel|Booting Linux on physical CPU|Linux version\s|Linux EFI stub:/i;
const LOGIN_PATTERN = /(?:^|[\r\n])[^\r\n]*login:\s*$|Welcome to (?:Ubuntu|Debian|Radxa|Armbian|Fedora|Arch Linux)|Reached target .*Multi-User|root@[^\r\n]*[#$]\s*$/im;

function matchingText(text: string) {
  return text
    .replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, "")
    .replaceAll("\0", "");
}

export function detectStartupMilestones(text: string, mode: BootloaderMode): StartupDetection {
  const clean = matchingText(text);
  const uboot = mode !== "uefi" && UBOOT_PATTERN.test(clean);
  const uefi = mode !== "uboot" && UEFI_PATTERN.test(clean);
  return {
    bootrom: BOOT_ROM_PATTERN.test(clean),
    bootloader: uefi ? "uefi" : uboot ? "uboot" : undefined,
    kernel: KERNEL_PATTERN.test(clean),
    login: LOGIN_PATTERN.test(clean),
  };
}
