/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "debugboard_control.h"
#include "debugboard_shell.h"

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

static struct k_work_delayable debugboard_shell_watchdog_work;

static void debugboard_shell_watchdog_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	debugboard_watchdog_note_cmdline_alive();
	(void)k_work_reschedule(&debugboard_shell_watchdog_work, K_MSEC(500));
}

static void debugboard_shell_bootloader_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	(void)debugboard_bootloader_now();
}

static K_WORK_DELAYABLE_DEFINE(debugboard_shell_bootloader_work,
			       debugboard_shell_bootloader_work_handler);

void debugboard_shell_watchdog_start(void)
{
	k_work_init_delayable(&debugboard_shell_watchdog_work,
			      debugboard_shell_watchdog_work_handler);
	(void)k_work_reschedule(&debugboard_shell_watchdog_work, K_NO_WAIT);
}

static int cmd_bootloader(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Entering RP2040 BOOTSEL in 250 ms...");
	(void)k_work_reschedule(&debugboard_shell_bootloader_work, K_MSEC(250));

	return 0;
}

SHELL_CMD_REGISTER(bootloader, NULL,
			   "Enter RP2040 BOOTSEL for UF2/picotool flashing.",
			   cmd_bootloader);
