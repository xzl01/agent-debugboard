#ifndef LINKR_DEBUGGER_SERIAL_RUNTIME_STUB_DEVICE_H_
#define LINKR_DEBUGGER_SERIAL_RUNTIME_STUB_DEVICE_H_

#include <stdbool.h>

struct device {
	const char *name;
	bool ready;
	void *data;
};

static inline bool device_is_ready(const struct device *dev)
{
	return dev != NULL && dev->ready;
}

#endif
