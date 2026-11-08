/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_usb_net.h"

#include "linkr_debugger_network.h"
#include "linkr_debugger_serial.h"
#include "linkr_debugger_webusb_url.h"

#include <errno.h>
#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbd_msg.h>

LOG_MODULE_REGISTER(linkr_debugger_usb_net, LOG_LEVEL_INF);

#define LINKR_DEBUGGER_USB_VID  sys_cpu_to_le16(0x2fe3U)
#define LINKR_DEBUGGER_USB_PID  sys_cpu_to_le16(0xDB01U)
#define LINKR_DEBUGGER_USB_POWER_MA 50U
#define LINKR_DEBUGGER_J4_CDC_NODE DT_CHOSEN(radxa_linkr_j4_cdc)
#define LINKR_DEBUGGER_WEBUSB_VENDOR_CODE 0x01U
#define LINKR_DEBUGGER_WEBUSB_LANDING_PAGE 0x0001U
#define LINKR_DEBUGGER_WEBUSB_GET_URL 0x0002U

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

static uint8_t webusb_url_descriptor[LINKR_DEBUGGER_WEBUSB_URL_DESCRIPTOR_MAX_LEN];
static size_t webusb_url_descriptor_len;

static const struct usb_bos_capability_lpm linkr_debugger_usb_ext = {
	.bLength = sizeof(struct usb_bos_capability_lpm),
	.bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
	.bDevCapabilityType = USB_BOS_CAPABILITY_EXTENSION,
	.bmAttributes = 0UL,
};

USBD_DESC_BOS_DEFINE(linkr_debugger_usb_ext_desc, sizeof(linkr_debugger_usb_ext),
		     &linkr_debugger_usb_ext);

struct linkr_debugger_webusb_bos_desc {
	struct usb_bos_platform_descriptor platform;
	struct usb_bos_capability_webusb capability;
} __packed;

static const struct linkr_debugger_webusb_bos_desc linkr_debugger_webusb_bos = {
	.platform = {
		.bLength = sizeof(struct linkr_debugger_webusb_bos_desc),
		.bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
		.bDevCapabilityType = USB_BOS_CAPABILITY_PLATFORM,
		.PlatformCapabilityUUID = {
			0x38, 0xB6, 0x08, 0x34, 0xA9, 0x09, 0xA0, 0x47,
			0x8B, 0xFD, 0xA0, 0x76, 0x88, 0x15, 0xB6, 0x65,
		},
	},
	.capability = {
		.bcdVersion = sys_cpu_to_le16(0x0100),
		.bVendorCode = LINKR_DEBUGGER_WEBUSB_VENDOR_CODE,
		.iLandingPage = LINKR_DEBUGGER_WEBUSB_LANDING_PAGE,
	},
};

static struct net_buf *linkr_debugger_webusb_to_host(
	const struct usbd_context *const ctx,
	const struct usb_setup_packet *const setup)
{
	struct net_buf *buf;
	size_t len;

	if (setup->RequestType.type != USB_REQTYPE_TYPE_VENDOR ||
	    setup->RequestType.recipient != USB_REQTYPE_RECIPIENT_DEVICE ||
	    setup->bRequest != LINKR_DEBUGGER_WEBUSB_VENDOR_CODE ||
	    setup->wIndex != LINKR_DEBUGGER_WEBUSB_GET_URL ||
	    setup->wValue != LINKR_DEBUGGER_WEBUSB_LANDING_PAGE ||
	    webusb_url_descriptor_len == 0U) {
		return NULL;
	}

	len = MIN((size_t)setup->wLength, webusb_url_descriptor_len);
	buf = usbd_ep_ctrl_data_in_alloc(ctx, len);
	if (buf == NULL) {
		return NULL;
	}

	net_buf_add_mem(buf, webusb_url_descriptor, len);
	return buf;
}

USBD_DESC_BOS_VREQ_DEFINE(linkr_debugger_webusb_desc,
			  sizeof(linkr_debugger_webusb_bos),
			  &linkr_debugger_webusb_bos,
			  LINKR_DEBUGGER_WEBUSB_VENDOR_CODE,
			  linkr_debugger_webusb_to_host, NULL);

static bool linkr_debugger_webusb_snapshot_url(void)
{
	struct linkr_debugger_net_identity identity;
	int ret;

	webusb_url_descriptor_len = 0U;
	ret = linkr_debugger_network_identity(&identity);
	if (ret < 0) {
		LOG_WRN("WebUSB URL identity unavailable (%d)", ret);
		return false;
	}

	ret = linkr_debugger_webusb_url_encode(&identity, webusb_url_descriptor,
					       sizeof(webusb_url_descriptor),
					       &webusb_url_descriptor_len);
	if (ret < 0) {
		LOG_WRN("WebUSB URL unavailable (%d)", ret);
		return false;
	}

	return true;
}

static void linkr_debugger_fix_code_triple(struct usbd_context *uds_ctx)
{
	usbd_device_set_code_triple(uds_ctx, USBD_SPEED_FS,
				    USB_BCC_MISCELLANEOUS, 0x02, 0x01);
}

static const struct device *const linkr_debugger_j4_cdc =
	DEVICE_DT_GET(LINKR_DEBUGGER_J4_CDC_NODE);

static void linkr_debugger_usbd_msg_cb(struct usbd_context *const ctx,
				   const struct usbd_msg *const msg)
{
	int ret = 0;

	ARG_UNUSED(ctx);

	LOG_INF("usb event: %s", usbd_msg_type_string(msg->type));

	switch (msg->type) {
	case USBD_MSG_CDC_ACM_LINE_CODING:
		if (msg->dev == linkr_debugger_j4_cdc) {
			ret = linkr_debugger_serial_cdc_line_coding(msg->dev);
		}
		break;
	case USBD_MSG_CDC_ACM_CONTROL_LINE_STATE:
		/* DTR/RTS changes never reconfigure PL011. Doing so here races the
		 * open-time line-coding notification and can stall long enough for the
		 * hardware watchdog to enter BOOTSEL.
		 */
		if (msg->dev == linkr_debugger_j4_cdc) {
			ret = linkr_debugger_serial_cdc_control_line(msg->dev);
		}
		break;
	case USBD_MSG_RESET:
		ret = linkr_debugger_serial_cdc_usb_event(
			linkr_debugger_j4_cdc, LINKR_DEBUGGER_SERIAL_USB_RESET,
			false);
		break;
	case USBD_MSG_CONFIGURATION:
		ret = linkr_debugger_serial_cdc_usb_event(
			linkr_debugger_j4_cdc,
			LINKR_DEBUGGER_SERIAL_USB_CONFIGURATION,
			msg->status != 0);
		break;
	default:
		break;
	}

	if (ret < 0 && ret != -ENODEV) {
		LOG_WRN("J4 serial USB event failed: %d", ret);
	}
}

static struct usbd_context *linkr_debugger_usbd_setup(void)
{
	bool webusb_available;
	int err;

	webusb_available = linkr_debugger_webusb_snapshot_url();

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

	if (webusb_available) {
		/* Chromium only probes WebUSB capabilities for USB 2.1 or newer. */
		err = usbd_device_set_bcd_usb(&linkr_debugger_usbd, USBD_SPEED_FS,
					      USB_SRN_2_1);
		if (err) {
			LOG_ERR("Failed to set USB 2.1 revision (%d)", err);
			return NULL;
		}

		err = usbd_add_descriptor(&linkr_debugger_usbd,
					  &linkr_debugger_usb_ext_desc);
		if (err) {
			LOG_ERR("Failed to add USB extension descriptor (%d)", err);
			return NULL;
		}

		err = usbd_add_descriptor(&linkr_debugger_usbd,
					  &linkr_debugger_webusb_desc);
		if (err) {
			LOG_ERR("Failed to add WebUSB descriptor (%d)", err);
			return NULL;
		}
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
