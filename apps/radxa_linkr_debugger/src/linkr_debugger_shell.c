/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "linkr_debugger_control.h"
#include "linkr_debugger_shell.h"

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

static struct k_work_delayable linkr_debugger_shell_watchdog_work;

static void linkr_debugger_shell_watchdog_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	linkr_debugger_watchdog_note_cmdline_alive();
	(void)k_work_reschedule(&linkr_debugger_shell_watchdog_work, K_MSEC(500));
}

static void linkr_debugger_shell_bootloader_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	(void)linkr_debugger_bootloader_now();
}

static K_WORK_DELAYABLE_DEFINE(linkr_debugger_shell_bootloader_work,
			       linkr_debugger_shell_bootloader_work_handler);

void linkr_debugger_shell_watchdog_start(void)
{
	k_work_init_delayable(&linkr_debugger_shell_watchdog_work,
			      linkr_debugger_shell_watchdog_work_handler);
	(void)k_work_reschedule(&linkr_debugger_shell_watchdog_work, K_NO_WAIT);
}

static int cmd_bootloader(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Entering RP2040 BOOTSEL in 250 ms...");
	(void)k_work_reschedule(&linkr_debugger_shell_bootloader_work, K_MSEC(250));

	return 0;
}

SHELL_CMD_REGISTER(bootloader, NULL,
			   "Enter RP2040 BOOTSEL for UF2/picotool flashing.",
			   cmd_bootloader);
