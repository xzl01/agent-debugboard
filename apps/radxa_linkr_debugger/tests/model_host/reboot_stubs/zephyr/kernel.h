/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef LINKR_DEBUGGER_REBOOT_STUB_KERNEL_H_
#define LINKR_DEBUGGER_REBOOT_STUB_KERNEL_H_

#include <stdint.h>

typedef int64_t k_timeout_t;

#define K_MSEC(value) ((k_timeout_t)(value))
#define ARG_UNUSED(value) (void)(value)
#define CODE_UNREACHABLE __builtin_unreachable()

struct k_work {
	void (*handler)(struct k_work *work);
};

struct k_work_delayable {
	struct k_work work;
};

#define K_WORK_DELAYABLE_DEFINE(name, work_handler)                              \
	struct k_work_delayable name = {                                         \
		.work = { .handler = (work_handler) },                           \
	}

int k_work_schedule(struct k_work_delayable *work, k_timeout_t delay);
int k_work_cancel_delayable(struct k_work_delayable *work);

#endif
