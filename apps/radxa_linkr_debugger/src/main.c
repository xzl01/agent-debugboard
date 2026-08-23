/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) xzl <xiangzelong@radxa.com>
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_control.h"
#include "linkr_debugger_capture_arena.h"
#include "linkr_debugger_config_service.h"
#include "linkr_debugger_config_store.h"
#include "linkr_debugger_http.h"
#include "linkr_debugger_logic_analyzer.h"
#include "linkr_debugger_network.h"
#include "linkr_debugger_task.h"
#include "linkr_debugger_ota.h"
#include "linkr_debugger_shell.h"
#include "linkr_debugger_sigrok_linkr.h"
#include "linkr_debugger_ws.h"
#include "linkr_debugger_usb_net.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/net/http/server.h>
#include <zephyr/sys/printk.h>
#include <zephyr/fatal.h>
#include <zephyr/toolchain.h>

LOG_MODULE_REGISTER(radxa_linkr_debugger, LOG_LEVEL_INF);

static const char *linkr_debugger_fatal_reason_str(unsigned int reason)
{
	switch (reason) {
	case K_ERR_CPU_EXCEPTION:
		return "CPU exception";
	case K_ERR_SPURIOUS_IRQ:
		return "spurious IRQ";
	case K_ERR_STACK_CHK_FAIL:
		return "stack overflow";
	case K_ERR_KERNEL_OOPS:
		return "kernel oops";
	case K_ERR_KERNEL_PANIC:
		return "kernel panic";
	default:
		return "unknown";
	}
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	const char *thread_name = k_thread_name_get(k_current_get());

	if (thread_name == NULL || thread_name[0] == '\0') {
		thread_name = "?";
	}

	printk("FATAL: %s (code=%u) thread=%s uptime=%lldms\n",
	       linkr_debugger_fatal_reason_str(reason), reason, thread_name,
	       (long long)k_uptime_get());

	LOG_PANIC();
	LOG_ERR("Halting system");
	k_fatal_halt(reason);
	CODE_UNREACHABLE;
}

int main(void)
{
	int ret;

	linkr_debugger_watchdog_boot_check();
	linkr_debugger_capture_arena_init();

	ret = linkr_debugger_control_init();
	if (ret < 0) {
		LOG_ERR("Board control init failed: %d", ret);
		return 0;
	}

	ret = linkr_debugger_config_store_init();
	if (ret != LINKR_DEBUGGER_CONFIG_STORE_OK) {
		LOG_WRN("Config store init non-OK: %d", ret);
	}

	ret = linkr_debugger_config_service_init();
	if (ret != LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		LOG_WRN("Config service init non-OK: %d", ret);
	}

	linkr_debugger_http_init();
	linkr_debugger_task_init();
	ret = linkr_debugger_ws_init();
	if (ret < 0) {
		LOG_ERR("WebSocket init failed: %d", ret);
		return 0;
	}

	ret = linkr_debugger_usb_net_init();
	if (ret < 0) {
		LOG_ERR("USB NCM init failed: %d", ret);
		return 0;
	}

	ret = linkr_debugger_network_init();
	if (ret < 0) {
		LOG_ERR("Network service init failed: %d", ret);
		return 0;
	}

	ret = linkr_debugger_logic_analyzer_init();
	if (ret < 0) {
		LOG_WRN("Logic analyzer init failed: %d (non-fatal)", ret);
	}

	ret = linkr_debugger_sigrok_linkr_init();
	if (ret < 0) {
		LOG_WRN("Sigrok Linkr init failed: %d (non-fatal)", ret);
	}

	ret = http_server_start();
	if (ret < 0) {
		LOG_ERR("HTTP server start failed: %d", ret);
		return 0;
	}

	linkr_debugger_shell_watchdog_start();

	ret = linkr_debugger_watchdog_supervisor_start();
	if (ret < 0) {
		LOG_ERR("Watchdog supervisor start failed: %d", ret);
		return 0;
	}

	if (IS_ENABLED(CONFIG_LINKR_DEBUGGER_OTA)) {
		linkr_debugger_ota_auto_confirm_ready();
	}

	LOG_INF("radxa-linkr-debugger controller ready over NCM HTTP with CDC ACM fallback");

	while (true) {
		linkr_debugger_watchdog_note_core_alive();
		k_sleep(K_MSEC(250));
	}
}
