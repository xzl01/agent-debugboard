#include <assert.h>
#include <errno.h>
#include <string.h>

#include "linkr_debugger_gpio_error.h"

static void test_output_eperm_maps_to_input_only(void)
{
	const struct linkr_debugger_gpio_error *mapping =
		linkr_debugger_gpio_configure_error(true, -EPERM);

	assert(strcmp(mapping->code, "input_only") == 0);
	assert(strstr(mapping->message, "input-only") != NULL);
	assert(mapping->forbidden);
}

static void test_output_other_negative_maps_to_configure_failed(void)
{
	const struct linkr_debugger_gpio_error *mapping =
		linkr_debugger_gpio_configure_error(true, -EIO);

	assert(strcmp(mapping->code, "configure_failed") == 0);
	assert(strcmp(mapping->message, "failed to configure GPIO") == 0);
	assert(!mapping->forbidden);
}

static void test_input_eperm_remains_configure_failed(void)
{
	const struct linkr_debugger_gpio_error *mapping =
		linkr_debugger_gpio_configure_error(false, -EPERM);

	assert(strcmp(mapping->code, "configure_failed") == 0);
	assert(strcmp(mapping->message, "failed to configure GPIO") == 0);
	assert(!mapping->forbidden);
}

static void test_input_other_negative_remains_configure_failed(void)
{
	const struct linkr_debugger_gpio_error *mapping =
		linkr_debugger_gpio_configure_error(false, -EIO);

	assert(strcmp(mapping->code, "configure_failed") == 0);
	assert(strcmp(mapping->message, "failed to configure GPIO") == 0);
	assert(!mapping->forbidden);
}

int main(void)
{
	test_output_eperm_maps_to_input_only();
	test_output_other_negative_maps_to_configure_failed();
	test_input_eperm_remains_configure_failed();
	test_input_other_negative_remains_configure_failed();
	return 0;
}
