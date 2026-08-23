/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_control.h"
#include "linkr_debugger_config_shell.h"
#include "linkr_debugger_task_shell.h"
#include "linkr_debugger_shell.h"

#include <errno.h>

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

	shell_print(sh, "Entering %s BOOTSEL in 250 ms...", linkr_debugger_mcu_name());
	(void)k_work_reschedule(&linkr_debugger_shell_bootloader_work, K_MSEC(250));

	return 0;
}

static int cmd_vin_get(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!linkr_debugger_vin_switch_available()) {
		shell_error(sh, "VIN switch is not available on this board");
		return -ENOTSUP;
	}

	shell_print(sh, "vin=%s", linkr_debugger_vin_route_name());
	return 0;
}

static int cmd_vin_set(const struct shell *sh, size_t argc, char **argv)
{
	enum linkr_debugger_vin_route route;
	int ret;

	if (argc != 2 || !linkr_debugger_parse_vin_route(argv[1], &route)) {
		shell_error(sh, "usage: vin set 1.8v|3.3v");
		return -EINVAL;
	}

	if (!linkr_debugger_vin_switch_available()) {
		shell_error(sh, "VIN switch is not available on this board");
		return -ENOTSUP;
	}

	ret = linkr_debugger_vin_route_set(route);
	if (ret < 0) {
		shell_error(sh, "failed to set VIN route: %d", ret);
		return ret;
	}

	shell_print(sh, "vin=%s", linkr_debugger_vin_route_name());
	return 0;
}

static int cmd_tf_wp_get(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "tf_wp=%s", linkr_debugger_tf_wp_route_name());
	return 0;
}

static int cmd_tf_wp_set(const struct shell *sh, size_t argc, char **argv)
{
	enum linkr_debugger_tf_wp_route route;
	int ret;

	if (argc != 2 || !linkr_debugger_parse_tf_wp_route(argv[1], &route)) {
		shell_error(sh, "usage: tf_wp set writable|protected");
		return -EINVAL;
	}

	ret = linkr_debugger_tf_wp_route_set(route);
	if (ret < 0) {
		shell_error(sh, "failed to set TF_WP route: %d", ret);
		return ret;
	}

	shell_print(sh, "tf_wp=%s", linkr_debugger_tf_wp_route_name());
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(vin_cmds,
	SHELL_CMD(get, NULL, "Get VIN voltage route.", cmd_vin_get),
	SHELL_CMD(set, NULL, "Set VIN voltage route: 1.8v or 3.3v.", cmd_vin_set),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(tf_wp_cmds,
	SHELL_CMD(get, NULL, "Get TF card write-protect route.", cmd_tf_wp_get),
	SHELL_CMD(set, NULL, "Set TF card write-protect route: writable or protected.", cmd_tf_wp_set),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(config_cmds,
	SHELL_CMD(show, NULL, "Show configuration status.", linkr_debugger_config_shell_show),
	SHELL_CMD(save, NULL, "Save configuration snapshot and apply it; optional --confirm.", linkr_debugger_config_shell_save),
	SHELL_CMD(clear, NULL, "Clear saved configuration; does not change current hardware.", linkr_debugger_config_shell_clear),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(task_cmds,
	SHELL_CMD(show, NULL, "Show stored task status.", linkr_debugger_task_shell_show),
	SHELL_CMD(clear, NULL, "Clear all stored tasks.", linkr_debugger_task_shell_clear),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(bootloader, NULL,
				   "Enter MCU BOOTSEL for UF2/picotool flashing.",
				   cmd_bootloader);
SHELL_CMD_REGISTER(vin, &vin_cmds, "Control VIN voltage route.", NULL);
SHELL_CMD_REGISTER(config, &config_cmds, "Control configuration snapshots.", NULL);
SHELL_CMD_REGISTER(tf_wp, &tf_wp_cmds, "Control TF card write-protect route.", NULL);
SHELL_CMD_REGISTER(task, &task_cmds, "Manage tasks stored in flash.", NULL);
