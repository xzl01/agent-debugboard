#include "linkr_debugger_capture_arbiter.h"
#include "linkr_debugger_config_service_internal.h"
#include "linkr_debugger_control.h"
#include "linkr_debugger_flash_arbiter.h"

#include <errno.h>
#include <string.h>

static int control_snapshot_get(void *context,
				struct linkr_debugger_control_snapshot *snapshot)
{
	(void)context;
	return linkr_debugger_control_snapshot_get(snapshot);
}

static int power_entry_set(const struct linkr_debugger_config_item_desc *item,
			   const struct linkr_debugger_config_entry *entry)
{
	const struct linkr_debugger_rail_desc *rail;
	const char *separator = strchr(item->id, '/');

	if (separator == NULL || separator[1] == '\0') {
		return -EINVAL;
	}
	rail = linkr_debugger_find_rail(separator + 1);
	if (rail == NULL) {
		return -ENODEV;
	}
	return linkr_debugger_power_output_set(
		rail, entry->value == LINKR_DEBUGGER_CONFIG_POWER_ON);
}

static int switch_entry_set(const struct linkr_debugger_config_item_desc *item,
			    const struct linkr_debugger_config_entry *entry)
{
	switch (item->item_id) {
	case LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID:
		return linkr_debugger_sd_route_set(
			entry->value == LINKR_DEBUGGER_CONFIG_SD_TARGET ?
				LINKR_DEBUGGER_SD_ROUTE_TARGET :
				LINKR_DEBUGGER_SD_ROUTE_USB_READER);
	case LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID:
		return linkr_debugger_usb_route_set(
			entry->value == LINKR_DEBUGGER_CONFIG_USB_PC ?
				LINKR_DEBUGGER_USB_ROUTE_PC :
				LINKR_DEBUGGER_USB_ROUTE_TARGET);
	case LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID:
		return linkr_debugger_tf_wp_route_set(
			entry->value == LINKR_DEBUGGER_CONFIG_TF_WP_WRITABLE ?
				LINKR_DEBUGGER_TF_WP_ROUTE_WRITABLE :
				LINKR_DEBUGGER_TF_WP_ROUTE_PROTECTED);
	case LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID:
		return linkr_debugger_vin_route_set(
			entry->value == LINKR_DEBUGGER_CONFIG_VIN_1V8 ?
				LINKR_DEBUGGER_VIN_ROUTE_1V8 :
				LINKR_DEBUGGER_VIN_ROUTE_3V3);
	default:
		return -EINVAL;
	}
}

static int gpio_entry_set(const struct linkr_debugger_config_item_desc *item,
			  const struct linkr_debugger_config_entry *entry)
{
	const struct linkr_debugger_safe_gpio_desc *gpio =
		linkr_debugger_find_safe_gpio_by_pin(item->item_id);

	if (gpio == NULL) {
		return -ERANGE;
	}
	if ((entry->value & LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT) == 0U) {
		if (entry->value != 0U) {
			return -EINVAL;
		}
		return linkr_debugger_gpio_set_input(gpio);
	}
	return linkr_debugger_gpio_set_output(
		gpio, (entry->value & LINKR_DEBUGGER_CONFIG_GPIO_LEVEL) != 0U);
}

static int control_apply_entry(void *context,
			       const struct linkr_debugger_config_entry *entry)
{
	const struct linkr_debugger_config_item_desc *item;
	bool requires_confirmation;

	(void)context;
	if (entry == NULL) {
		return -EINVAL;
	}
	item = linkr_debugger_config_find_item(entry->domain, entry->item_id);
	if (item == NULL) {
		return -EINVAL;
	}
	if (linkr_debugger_config_classify_entry(entry, &requires_confirmation) !=
	    LINKR_DEBUGGER_CONFIG_CODEC_OK) {
		return -EINVAL;
	}

	switch (item->domain) {
	case LINKR_DEBUGGER_CONFIG_DOMAIN_POWER:
		return power_entry_set(item, entry);
	case LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH:
		return switch_entry_set(item, entry);
	case LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO:
		return gpio_entry_set(item, entry);
	default:
		return -EINVAL;
	}
}

static enum linkr_debugger_config_store_result store_status_get(
	void *context, struct linkr_debugger_config_store_status *status)
{
	(void)context;
	return linkr_debugger_config_store_status_get(status);
}

static enum linkr_debugger_config_store_result store_snapshot_get(
	void *context, struct linkr_debugger_config_snapshot *snapshot)
{
	(void)context;
	return linkr_debugger_config_store_snapshot_get(snapshot);
}

static enum linkr_debugger_config_store_result store_save(
	void *context, const struct linkr_debugger_config_snapshot *snapshot)
{
	(void)context;
	return linkr_debugger_config_store_save(snapshot);
}

static enum linkr_debugger_config_store_result store_clear(void *context)
{
	(void)context;
	return linkr_debugger_config_store_clear();
}

static bool capture_try_acquire(void *context)
{
	(void)context;
	return linkr_debugger_capture_arbiter_try_acquire(
		LINKR_DEBUGGER_CAPTURE_OWNER_PERSISTENT_CONFIG);
}

static bool capture_release(void *context)
{
	(void)context;
	return linkr_debugger_capture_arbiter_release(
		LINKR_DEBUGGER_CAPTURE_OWNER_PERSISTENT_CONFIG);
}

static bool flash_try_acquire(void *context)
{
	(void)context;
	return linkr_debugger_flash_arbiter_try_acquire(
		LINKR_DEBUGGER_FLASH_OWNER_CONFIG);
}

static bool flash_release(void *context)
{
	(void)context;
	return linkr_debugger_flash_arbiter_release(
		LINKR_DEBUGGER_FLASH_OWNER_CONFIG);
}

const struct linkr_debugger_config_service_ops
	linkr_debugger_config_service_production_ops = {
		.control_snapshot_get = control_snapshot_get,
		.control_apply_entry = control_apply_entry,
		.store_status_get = store_status_get,
		.store_snapshot_get = store_snapshot_get,
		.store_save = store_save,
		.store_clear = store_clear,
		.capture_try_acquire = capture_try_acquire,
		.capture_release = capture_release,
		.flash_try_acquire = flash_try_acquire,
		.flash_release = flash_release,
	};
