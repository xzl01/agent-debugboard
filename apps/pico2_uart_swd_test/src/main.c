/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runs the 2x CDC ACM UART bridges and the CMSIS-DAP SWD backend. No UART
 * console is enabled by default; logs are compiled in for debug builds.
 */

#include <zephyr/kernel.h>
#include <zephyr/dap/dap_link.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/uart/uart_bridge.h>

#include <sample_usbd.h>

LOG_MODULE_REGISTER(pico2_uart_swd_test, LOG_LEVEL_INF);

DAP_LINK_CONTEXT_DEFINE(pico2_dap_ctx, DEVICE_DT_GET_ONE(zephyr_swdp_gpio));

#define DEVICE_DT_GET_COMMA(node_id) DEVICE_DT_GET(node_id),

static const struct device *const uart_bridges[] = {
	DT_FOREACH_STATUS_OKAY(zephyr_uart_bridge, DEVICE_DT_GET_COMMA)
};

static void pico2_usb_msg_cb(struct usbd_context *const usbd_ctx,
			     const struct usbd_msg *const msg)
{
	if (msg->type == USBD_MSG_CDC_ACM_LINE_CODING ||
	    msg->type == USBD_MSG_CDC_ACM_CONTROL_LINE_STATE) {
		for (size_t i = 0; i < ARRAY_SIZE(uart_bridges); i++) {
			uart_bridge_settings_update(msg->dev, uart_bridges[i]);
		}
	}
}

int main(void)
{
	struct usbd_context *usbd;
	int ret;

	ret = dap_link_init(&pico2_dap_ctx);
	if (ret != 0) {
		LOG_ERR("DAP init failed: %d", ret);
		return ret;
	}

	ret = dap_link_backend_usb_init(&pico2_dap_ctx);
	if (ret != 0) {
		LOG_ERR("DAP USB backend init failed: %d", ret);
		return ret;
	}

	usbd = sample_usbd_init_device(pico2_usb_msg_cb);
	if (usbd == NULL) {
		LOG_ERR("USB setup failed");
		return -ENODEV;
	}

	if (!usbd_can_detect_vbus(usbd)) {
		ret = usbd_enable(usbd);
		if (ret != 0) {
			LOG_ERR("USB enable failed: %d", ret);
			return ret;
		}
	}

	LOG_INF("Pico2 2xUART+SWD test firmware ready");
	k_sleep(K_FOREVER);
	return 0;
}
