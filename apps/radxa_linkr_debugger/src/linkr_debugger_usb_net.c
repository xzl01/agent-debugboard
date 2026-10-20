/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_usb_net.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/uart/uart_bridge.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbd_msg.h>

LOG_MODULE_REGISTER(linkr_debugger_usb_net, LOG_LEVEL_INF);

#define LINKR_DEBUGGER_USB_VID  sys_cpu_to_le16(0x2fe3U)
#define LINKR_DEBUGGER_USB_PID  sys_cpu_to_le16(0xDB01U)
#define LINKR_DEBUGGER_USB_POWER_MA 50U
#define LINKR_DEBUGGER_J4_UART_MIN_BAUD 2400U
#define LINKR_DEBUGGER_J4_UART_MAX_BAUD (UINT32_MAX / 16U)

USBD_DEVICE_DEFINE(linkr_debugger_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   LINKR_DEBUGGER_USB_VID, LINKR_DEBUGGER_USB_PID);

USBD_DESC_LANG_DEFINE(linkr_debugger_lang);
USBD_DESC_MANUFACTURER_DEFINE(linkr_debugger_mfr, "Radxa");
USBD_DESC_PRODUCT_DEFINE(linkr_debugger_product, "Radxa Linkr Debugger");
USBD_DESC_SERIAL_NUMBER_DEFINE(linkr_debugger_sn);
USBD_DESC_CONFIG_DEFINE(linkr_debugger_fs_cfg_desc, "FS Configuration");

USBD_CONFIGURATION_DEFINE(linkr_debugger_fs_config, 0U, LINKR_DEBUGGER_USB_POWER_MA,
			  &linkr_debugger_fs_cfg_desc);

static void linkr_debugger_fix_code_triple(struct usbd_context *uds_ctx)
{
	usbd_device_set_code_triple(uds_ctx, USBD_SPEED_FS,
				    USB_BCC_MISCELLANEOUS, 0x02, 0x01);
}

#define DEVICE_DT_GET_COMMA(node_id) DEVICE_DT_GET(node_id),

static const struct device *const uart_bridges[] = {
	DT_FOREACH_STATUS_OKAY(zephyr_uart_bridge, DEVICE_DT_GET_COMMA)
};

static void linkr_debugger_usbd_msg_cb(struct usbd_context *const ctx,
				   const struct usbd_msg *const msg)
{
	ARG_UNUSED(ctx);

	LOG_INF("usb event: %s", usbd_msg_type_string(msg->type));

	/* DTR/RTS changes do not alter the UART format. Reconfiguring PL011 on
	 * every CONTROL_LINE_STATE event can race consecutive open-time events
	 * (stale line coding, new line coding, then DTR) and wedge the bridge long
	 * enough for the hardware watchdog to enter BOOTSEL. Apply settings only
	 * after the host has delivered a new line-coding tuple.
	 */
	if (msg->type == USBD_MSG_CDC_ACM_LINE_CODING) {
		struct uart_config config;

		if (uart_config_get(msg->dev, &config) < 0 ||
		    config.baudrate < LINKR_DEBUGGER_J4_UART_MIN_BAUD ||
		    config.baudrate > LINKR_DEBUGGER_J4_UART_MAX_BAUD) {
			LOG_WRN("ignoring unsafe CDC UART line coding");
			return;
		}
		for (size_t i = 0; i < ARRAY_SIZE(uart_bridges); i++) {
			uart_bridge_settings_update(msg->dev, uart_bridges[i]);
		}
	}
}

static struct usbd_context *linkr_debugger_usbd_setup(void)
{
	int err;

	err = usbd_add_descriptor(&linkr_debugger_usbd, &linkr_debugger_lang);
	if (err) {
		LOG_ERR("Failed to add language descriptor (%d)", err);
		return NULL;
	}

	err = usbd_add_descriptor(&linkr_debugger_usbd, &linkr_debugger_mfr);
	if (err) {
		LOG_ERR("Failed to add manufacturer descriptor (%d)", err);
		return NULL;
	}

	err = usbd_add_descriptor(&linkr_debugger_usbd, &linkr_debugger_product);
	if (err) {
		LOG_ERR("Failed to add product descriptor (%d)", err);
		return NULL;
	}

	err = usbd_add_descriptor(&linkr_debugger_usbd, &linkr_debugger_sn);
	if (err) {
		LOG_ERR("Failed to add serial descriptor (%d)", err);
		return NULL;
	}

	err = usbd_add_configuration(&linkr_debugger_usbd, USBD_SPEED_FS,
				     &linkr_debugger_fs_config);
	if (err) {
		LOG_ERR("Failed to add full-speed configuration (%d)", err);
		return NULL;
	}

	err = usbd_register_all_classes(&linkr_debugger_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		LOG_ERR("Failed to register USB classes (%d)", err);
		return NULL;
	}

	linkr_debugger_fix_code_triple(&linkr_debugger_usbd);

	err = usbd_msg_register_cb(&linkr_debugger_usbd, linkr_debugger_usbd_msg_cb);
	if (err) {
		LOG_WRN("Failed to register USB message callback (%d)", err);
	}

	err = usbd_init(&linkr_debugger_usbd);
	if (err) {
		LOG_ERR("Failed to initialize USB device (%d)", err);
		return NULL;
	}

	return &linkr_debugger_usbd;
}

int linkr_debugger_usb_net_init(void)
{
	struct usbd_context *usbd;
	int ret;

	usbd = linkr_debugger_usbd_setup();
	if (usbd == NULL) {
		return -ENODEV;
	}

	ret = usbd_enable(usbd);
	if (ret < 0) {
		return ret;
	}

	return 0;
}
