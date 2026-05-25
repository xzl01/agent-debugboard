/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "linkr_debugger_usb_net.h"

#include "linkr_debugger_network.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_config.h>
#include <zephyr/net/net_if.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbd_msg.h>

LOG_MODULE_REGISTER(linkr_debugger_usb_net, LOG_LEVEL_INF);

#define LINKR_DEBUGGER_USB_VID  sys_cpu_to_le16(0x2fe3U)
#define LINKR_DEBUGGER_USB_PID  sys_cpu_to_le16(0xDB01U)
#define LINKR_DEBUGGER_USB_POWER_MA 50U

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

static void linkr_debugger_usbd_msg_cb(struct usbd_context *const ctx,
				   const struct usbd_msg *const msg)
{
	ARG_UNUSED(ctx);

	LOG_INF("usb event: %s", usbd_msg_type_string(msg->type));
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

	err = usbd_init(&linkr_debugger_usbd);
	if (err) {
		LOG_ERR("Failed to initialize USB device (%d)", err);
		return NULL;
	}

	err = usbd_msg_register_cb(&linkr_debugger_usbd, linkr_debugger_usbd_msg_cb);
	if (err) {
		LOG_WRN("Failed to register USB message callback (%d)", err);
	}

	return &linkr_debugger_usbd;
}

int linkr_debugger_usb_net_init(void)
{
	struct usbd_context *usbd;
	struct net_if *iface;
	int ret;

	usbd = linkr_debugger_usbd_setup();
	if (usbd == NULL) {
		return -ENODEV;
	}

	ret = usbd_enable(usbd);
	if (ret < 0) {
		return ret;
	}

	ret = linkr_debugger_network_get_ncm_iface(&iface);
	if (ret < 0) {
		return ret;
	}

	net_if_set_default(iface);

	ret = net_config_init_by_iface(iface, "Initializing network",
				      NET_CONFIG_NEED_IPV4,
				      CONFIG_NET_CONFIG_INIT_TIMEOUT * MSEC_PER_SEC);
	if (ret < 0) {
		return ret;
	}

	return 0;
}
