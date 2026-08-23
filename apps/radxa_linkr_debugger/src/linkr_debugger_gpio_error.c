#include "linkr_debugger_gpio_error.h"

#include <errno.h>

static const struct linkr_debugger_gpio_error input_only_error = {
	.code = "input_only",
	.message = "GPIO is input-only and cannot be configured as an output",
	.forbidden = true,
};

static const struct linkr_debugger_gpio_error configure_failed_error = {
	.code = "configure_failed",
	.message = "failed to configure GPIO",
	.forbidden = false,
};

const struct linkr_debugger_gpio_error *
linkr_debugger_gpio_configure_error(bool output, int error)
{
	if (output && error == -EPERM) {
		return &input_only_error;
	}

	return &configure_failed_error;
}
