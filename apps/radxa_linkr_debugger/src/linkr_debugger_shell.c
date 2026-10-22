/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_reboot.h"

#if !defined(LINKR_DEBUGGER_SHELL_REBOOT_HOST_TEST)
#include "linkr_debugger_build_info.h"
#include "linkr_debugger_control.h"
#include "linkr_debugger_config_shell.h"
#include "linkr_debugger_net_settings.h"
#include "linkr_debugger_network.h"
#include "linkr_debugger_pd.h"
#include "linkr_debugger_task_shell.h"
#include "linkr_debugger_shell.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

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

#endif /* !LINKR_DEBUGGER_SHELL_REBOOT_HOST_TEST */

static int cmd_reboot(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	(void)argv;
	if (argc != 1U) {
		shell_error(sh, "usage: reboot");
		return -EINVAL;
	}

	ret = linkr_debugger_reboot_schedule();
	if (ret == -EALREADY) {
		shell_error(sh, "reboot pending: normal cold reboot already scheduled (not BOOTSEL)");
		return ret;
	}
	if (ret < 0) {
		shell_error(sh, "failed to schedule normal cold reboot: %d", ret);
		return ret;
	}

	shell_print(sh, "normal cold reboot in %u ms (not BOOTSEL)",
		    LINKR_DEBUGGER_REBOOT_DELAY_MS);
	return 0;
}

#if !defined(LINKR_DEBUGGER_SHELL_REBOOT_HOST_TEST)
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

enum linkr_debugger_uname_field {
	LINKR_DEBUGGER_UNAME_SYSNAME = BIT(0),
	LINKR_DEBUGGER_UNAME_NODE = BIT(1),
	LINKR_DEBUGGER_UNAME_RELEASE = BIT(2),
	LINKR_DEBUGGER_UNAME_VERSION = BIT(3),
	LINKR_DEBUGGER_UNAME_MACHINE = BIT(4),
	LINKR_DEBUGGER_UNAME_PROCESSOR = BIT(5),
	LINKR_DEBUGGER_UNAME_PLATFORM = BIT(6),
};

static void linkr_debugger_uname_print(const struct shell *sh,
				       unsigned int fields)
{
	if ((fields & LINKR_DEBUGGER_UNAME_SYSNAME) != 0U) {
		shell_fprintf(sh, SHELL_NORMAL, "%s ",
			      linkr_debugger_build_sysname());
	}
	if ((fields & LINKR_DEBUGGER_UNAME_NODE) != 0U) {
		shell_fprintf(sh, SHELL_NORMAL, "%s ",
			      linkr_debugger_build_nodename());
	}
	if ((fields & LINKR_DEBUGGER_UNAME_RELEASE) != 0U) {
		shell_fprintf(sh, SHELL_NORMAL, "%s ",
			      linkr_debugger_build_release());
	}
	if ((fields & LINKR_DEBUGGER_UNAME_VERSION) != 0U) {
		shell_fprintf(sh, SHELL_NORMAL, "%s ",
			      linkr_debugger_build_version());
	}
	if ((fields & LINKR_DEBUGGER_UNAME_MACHINE) != 0U) {
		shell_fprintf(sh, SHELL_NORMAL, "%s ",
			      linkr_debugger_build_machine());
	}
	if ((fields & LINKR_DEBUGGER_UNAME_PROCESSOR) != 0U) {
		shell_fprintf(sh, SHELL_NORMAL, "%s ",
			      linkr_debugger_build_processor());
	}
	if ((fields & LINKR_DEBUGGER_UNAME_PLATFORM) != 0U) {
		shell_fprintf(sh, SHELL_NORMAL, "%s",
			      linkr_debugger_build_platform());
	}
	shell_fprintf(sh, SHELL_NORMAL, "\n");
}

static int cmd_uname(const struct shell *sh, size_t argc, char **argv)
{
	const unsigned int all_fields = LINKR_DEBUGGER_UNAME_SYSNAME |
		LINKR_DEBUGGER_UNAME_NODE | LINKR_DEBUGGER_UNAME_RELEASE |
		LINKR_DEBUGGER_UNAME_VERSION | LINKR_DEBUGGER_UNAME_MACHINE |
		LINKR_DEBUGGER_UNAME_PROCESSOR | LINKR_DEBUGGER_UNAME_PLATFORM;
	unsigned int fields = 0U;
	size_t index;

	if (argc == 2U && strcmp(argv[1], "--help") == 0) {
		shell_print(sh, "Usage: uname [-asnrmvpi]");
		shell_print(sh, "  -a  all information");
		shell_print(sh, "  -s  system name");
		shell_print(sh, "  -n  node name");
		shell_print(sh, "  -r  release");
		shell_print(sh, "  -v  version and build id");
		shell_print(sh, "  -m  machine hardware name");
		shell_print(sh, "  -p  processor type");
		shell_print(sh, "  -i  hardware platform");
		return 0;
	}

	for (index = 1U; index < argc; ++index) {
		const char *arg = argv[index];

		if (arg[0] != '-' || arg[1] == '\0') {
			shell_error(sh, "uname: extra operand %s", arg);
			shell_print(sh, "Try 'uname --help' for more information.");
			return -EINVAL;
		}

		for (++arg; *arg != '\0'; ++arg) {
			switch (*arg) {
			case 'a':
				fields = all_fields;
				break;
			case 's':
			case 'o':
				fields |= LINKR_DEBUGGER_UNAME_SYSNAME;
				break;
			case 'n':
				fields |= LINKR_DEBUGGER_UNAME_NODE;
				break;
			case 'r':
				fields |= LINKR_DEBUGGER_UNAME_RELEASE;
				break;
			case 'v':
				fields |= LINKR_DEBUGGER_UNAME_VERSION;
				break;
			case 'm':
				fields |= LINKR_DEBUGGER_UNAME_MACHINE;
				break;
			case 'p':
				fields |= LINKR_DEBUGGER_UNAME_PROCESSOR;
				break;
			case 'i':
				fields |= LINKR_DEBUGGER_UNAME_PLATFORM;
				break;
			default:
				shell_error(sh, "uname: illegal option -- %c",
					    *arg);
				shell_print(sh,
					    "Try 'uname --help' for more information.");
				return -EINVAL;
			}
		}
	}

	if (fields == 0U) {
		fields = LINKR_DEBUGGER_UNAME_SYSNAME;
	}

	linkr_debugger_uname_print(sh, fields);
	return 0;
}

static void linkr_debugger_shell_print_ip_identity(const struct shell *sh)
{
	struct linkr_debugger_net_identity identity;
	char mac[LINKR_DEBUGGER_NET_MAC_STR_LEN];
	char network[LINKR_DEBUGGER_NET_NETWORK_STR_LEN];
	char ip[LINKR_DEBUGGER_NET_IPV4_STR_LEN];
	char dhcp_base[LINKR_DEBUGGER_NET_IPV4_STR_LEN];
	char url[LINKR_DEBUGGER_NET_URL_STR_LEN];

	if (linkr_debugger_network_identity(&identity) < 0 ||
	    linkr_debugger_net_format_mac(identity.mac, mac, sizeof(mac)) < 0 ||
	    linkr_debugger_net_format_network(identity.network, network,
					      sizeof(network)) < 0 ||
	    linkr_debugger_net_format_ipv4(identity.network,
					   LINKR_DEBUGGER_NET_DEVICE_HOST,
					   ip, sizeof(ip)) < 0 ||
	    linkr_debugger_net_format_ipv4(identity.network,
					   LINKR_DEBUGGER_NET_DHCP_BASE_HOST,
					   dhcp_base, sizeof(dhcp_base)) < 0 ||
	    linkr_debugger_net_format_url(identity.network, url, sizeof(url)) < 0) {
		shell_error(sh, "identity format failed");
		return;
	}

	shell_print(sh, "mac=%s origin=%s network=%s ip=%s dhcp_base=%s url=%s",
		    mac,
		    identity.origin == LINKR_DEBUGGER_NET_ORIGIN_MANUAL ? "manual" :
									  "derived",
		    network, ip, dhcp_base, url);
}

static int cmd_ip(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argv);

	if (argc != 1U) {
		shell_error(sh, "unknown ip subcommand");
		return -EINVAL;
	}
	linkr_debugger_shell_print_ip_identity(sh);
	return 0;
}

static int cmd_ip_mac(const struct shell *sh, size_t argc, char **argv)
{
	struct linkr_debugger_net_identity identity;
	uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN];
	bool reconnect_required;
	int ret;

	if (argc != 2U) {
		shell_error(sh, "usage: ip mac <aa:bb:cc:dd:ee:ff>");
		return -EINVAL;
	}

	ret = linkr_debugger_net_parse_mac(argv[1], mac);
	if (ret < 0) {
		shell_error(sh, "invalid locally administered unicast mac");
		return ret;
	}

	ret = linkr_debugger_network_identity(&identity);
	if (ret < 0) {
		shell_error(sh, "identity unavailable: %d", ret);
		return ret;
	}

	memcpy(identity.mac, mac, sizeof(mac));
	identity.origin = LINKR_DEBUGGER_NET_ORIGIN_MANUAL;

	ret = linkr_debugger_network_set_identity_with_result(&identity,
							     &reconnect_required);
	if (ret < 0) {
		shell_error(sh, "MAC set failed: %d", ret);
		return ret;
	}

	linkr_debugger_shell_print_ip_identity(sh);
	if (reconnect_required) {
		shell_print(sh, "Reconnect USB NCM or renew DHCP");
	}
	return 0;
}

static int cmd_ip_network(const struct shell *sh, size_t argc, char **argv)
{
	struct linkr_debugger_net_identity identity;
	uint32_t network;
	bool reconnect_required;
	int ret;

	if (argc != 2U) {
		shell_error(sh, "usage: ip network <a.b.c.d[/30]>");
		return -EINVAL;
	}

	ret = linkr_debugger_net_parse_network(argv[1], &network);
	if (ret < 0) {
		shell_error(sh, "invalid private /30 network base");
		return ret;
	}

	ret = linkr_debugger_network_identity(&identity);
	if (ret < 0) {
		shell_error(sh, "identity unavailable: %d", ret);
		return ret;
	}

	identity.network = network;
	identity.origin = LINKR_DEBUGGER_NET_ORIGIN_MANUAL;

	ret = linkr_debugger_network_set_identity_with_result(&identity,
							     &reconnect_required);
	if (ret < 0) {
		shell_error(sh, "identity set failed: %d", ret);
		return ret;
	}

	linkr_debugger_shell_print_ip_identity(sh);
	if (reconnect_required) {
		shell_print(sh, "Reconnect USB NCM or renew DHCP");
	}
	return 0;
}

#if defined(CONFIG_LINKR_DEBUGGER_FAULT_INJECTION)
static int cmd_watchdog_fault_injection(const struct shell *sh,
					 size_t argc, char **argv)
{
	int ret;

	if (argc != 2) {
		shell_error(sh, "usage: watchdog fault-injection arm|disarm|status");
		return -EINVAL;
	}

	if (strcmp(argv[1], "arm") == 0) {
		ret = linkr_debugger_watchdog_fault_injection_arm();
		if (ret < 0) {
			shell_error(sh, "watchdog fault injection rejected: %d", ret);
			return ret;
		}
		shell_print(sh, "watchdog fault injection armed");
		return 0;
	}
	if (strcmp(argv[1], "disarm") == 0) {
		ret = linkr_debugger_watchdog_fault_injection_disarm();
		if (ret < 0) {
			shell_error(sh, "watchdog fault injection unavailable: %d", ret);
			return ret;
		}
		shell_print(sh, "watchdog fault injection disarmed");
		return 0;
	}
	if (strcmp(argv[1], "status") == 0) {
		shell_print(sh, "watchdog fault injection available=%s armed=%s",
			    linkr_debugger_watchdog_fault_injection_available() ?
				    "true" : "false",
			    linkr_debugger_watchdog_fault_injection_armed() ?
				    "true" : "false");
		return 0;
	}

	shell_error(sh, "usage: watchdog fault-injection arm|disarm|status");
	return -EINVAL;
}
#endif

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

static void linkr_debugger_shell_print_pd(const struct shell *sh)
{
	struct linkr_debugger_pd_status status;
	int ret = linkr_debugger_pd_get_status(&status);

	if (ret < 0) {
		shell_error(sh, "pd status failed: %d", ret);
		return;
	}

	shell_print(sh,
		    "pd available=%d mode=%s voltage_mv=%u gear=%u pps_mv=%u "
		    "pd=%d qc2=%d qc3=%d bc=%d epr=%d avs=%d max_current_ma=%u error=%d",
		    status.available,
		    status.mode == LINKR_DEBUGGER_PD_MODE_PPS ? "pps" : "fixed",
		    status.voltage_mv, status.gear, status.pps_mv,
		    status.pd_active, status.qc2, status.qc3, status.bc,
		    status.epr, status.avs, status.max_current_ma,
		    status.last_error);
}

static int cmd_pd(const struct shell *sh, size_t argc, char **argv)
{
	const char *value;
	char *end;
	unsigned long parsed;
	uint16_t mv;
	bool pps = false;
	int ret;

	if (argc == 1 || strcmp(argv[1], "status") == 0) {
		linkr_debugger_shell_print_pd(sh);
		return 0;
	}

	if (strcmp(argv[1], "fixed") == 0 || strcmp(argv[1], "pps") == 0) {
		pps = strcmp(argv[1], "pps") == 0;
		if (argc != 3) {
			shell_error(sh, "usage: pd [status] | pd fixed <mv> | pd pps <mv>");
			return -EINVAL;
		}
		value = argv[2];
	} else {
		value = argv[1];
	}
	for (const unsigned char *cursor = (const unsigned char *)value;
	     *cursor != '\0'; cursor++) {
		if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9') {
			shell_error(sh, "usage: pd [status] | pd fixed <mv> | pd pps <mv>");
			return -EINVAL;
		}
	}

	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (end == value || *end != '\0' || errno == ERANGE || parsed > 20000UL) {
		shell_error(sh, "usage: pd [status] | pd fixed <mv> | pd pps <mv>");
		return -EINVAL;
	}

	mv = parsed <= 20UL ? (uint16_t)(parsed * 1000UL) : (uint16_t)parsed;
	ret = pps ? linkr_debugger_pd_set_pps(mv) :
		    linkr_debugger_pd_set_fixed(mv);
	if (ret < 0) {
		shell_error(sh, "pd set failed: %d", ret);
		return ret;
	}

	linkr_debugger_shell_print_pd(sh);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ip_cmds,
	SHELL_CMD_ARG(mac, NULL, "Set the board MAC address (aa:bb:cc:dd:ee:ff).",
		      cmd_ip_mac, 2, 0),
	SHELL_CMD_ARG(network, NULL, "Set the board /30 network base (a.b.c.d or a.b.c.d/30).",
		      cmd_ip_network, 2, 0),
	SHELL_SUBCMD_SET_END);

#if defined(CONFIG_LINKR_DEBUGGER_FAULT_INJECTION)
SHELL_STATIC_SUBCMD_SET_CREATE(watchdog_cmds,
	SHELL_CMD(fault-injection, NULL, "Arm/disarm/status the HIL watchdog fault-injection hook.",
		  cmd_watchdog_fault_injection),
	SHELL_SUBCMD_SET_END);
#endif

SHELL_CMD_REGISTER(bootloader, NULL,
				   "Enter MCU BOOTSEL for UF2/picotool flashing.",
				   cmd_bootloader);
SHELL_CMD_ARG_REGISTER(reboot, NULL, "Normal cold reboot (not BOOTSEL).",
		       cmd_reboot, 1, 0);
SHELL_CMD_REGISTER(uname, NULL, "Print system information and firmware build id.",
		   cmd_uname);
SHELL_CMD_REGISTER(config, &config_cmds, "Control configuration snapshots.", NULL);
SHELL_CMD_REGISTER(tf_wp, &tf_wp_cmds, "Control TF card write-protect route.", NULL);
SHELL_CMD_REGISTER(task, &task_cmds, "Manage tasks stored in flash.", NULL);
SHELL_CMD_REGISTER(ip, &ip_cmds, "Show or set the board network identity.", cmd_ip);
SHELL_CMD_REGISTER(net, &ip_cmds, "Alias for 'ip'.", cmd_ip);
SHELL_CMD_REGISTER(pd, NULL,
		   "Show or set PD voltage (fixed 5/9/12/15/20 V; PPS 3300-21000 mV/100 mV).",
		   cmd_pd);
#if defined(CONFIG_LINKR_DEBUGGER_FAULT_INJECTION)
SHELL_CMD_REGISTER(watchdog, &watchdog_cmds, "Control watchdog diagnostics and fault injection.", NULL);
#endif
#endif /* !LINKR_DEBUGGER_SHELL_REBOOT_HOST_TEST */
