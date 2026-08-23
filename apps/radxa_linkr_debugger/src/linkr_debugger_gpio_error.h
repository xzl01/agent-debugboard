#ifndef RADXA_LINKR_DEBUGGER_GPIO_ERROR_H_
#define RADXA_LINKR_DEBUGGER_GPIO_ERROR_H_

#include <stdbool.h>

struct linkr_debugger_gpio_error {
	const char *code;
	const char *message;
	bool forbidden;
};

const struct linkr_debugger_gpio_error *
linkr_debugger_gpio_configure_error(bool output, int error);

#endif
