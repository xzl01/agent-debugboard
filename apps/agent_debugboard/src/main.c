/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * agent-debugboard RP2040 controller firmware.
 */

#include "debugboard_control.h"
#include "debugboard_http.h"
#include "debugboard_network.h"
#include "debugboard_shell.h"
#include "debugboard_ws.h"
#include "debugboard_usb_net.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/net/http/server.h>
#include <zephyr/sys/printk.h>
#include <zephyr/fatal.h>
#include <zephyr/toolchain.h>

LOG_MODULE_REGISTER(agent_debugboard, LOG_LEVEL_INF);

static const char *debugboard_fatal_reason_str(unsigned int reason)
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
	       debugboard_fatal_reason_str(reason), reason, thread_name,
	       (long long)k_uptime_get());

	LOG_PANIC();
	LOG_ERR("Halting system");
	k_fatal_halt(reason);
	CODE_UNREACHABLE;
}

int main(void)
{
	int ret;

	debugboard_watchdog_boot_check();

	ret = debugboard_control_init();
	if (ret < 0) {
		LOG_ERR("Board control init failed: %d", ret);
		return 0;
	}

	debugboard_http_init();
	ret = debugboard_ws_init();
	if (ret < 0) {
		LOG_ERR("WebSocket init failed: %d", ret);
		return 0;
	}

	ret = debugboard_usb_net_init();
	if (ret < 0) {
		LOG_ERR("USB NCM init failed: %d", ret);
		return 0;
	}

	ret = debugboard_network_init();
	if (ret < 0) {
		LOG_ERR("Network service init failed: %d", ret);
		return 0;
	}

	ret = http_server_start();
	if (ret < 0) {
		LOG_ERR("HTTP server start failed: %d", ret);
		return 0;
	}

	debugboard_shell_watchdog_start();

	ret = debugboard_watchdog_supervisor_start();
	if (ret < 0) {
		LOG_ERR("Watchdog supervisor start failed: %d", ret);
		return 0;
	}

	LOG_INF("agent-debugboard controller ready over NCM HTTP with CDC ACM fallback");

	while (true) {
		debugboard_watchdog_note_core_alive();
		k_sleep(K_MSEC(250));
	}
}
