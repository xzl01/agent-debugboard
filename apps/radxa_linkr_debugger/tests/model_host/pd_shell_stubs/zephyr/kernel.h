/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef LINKR_DEBUGGER_PD_SHELL_TEST_KERNEL_H_
#define LINKR_DEBUGGER_PD_SHELL_TEST_KERNEL_H_

#include <stdint.h>

struct k_work {
	void (*handler)(struct k_work *work);
};

struct k_work_delayable {
	struct k_work work;
};

#define K_WORK_DELAYABLE_DEFINE(name, work_handler) \
	struct k_work_delayable name = { \
		.work = { .handler = (work_handler) }, \
	}

#define K_MSEC(value) (value)
#define K_NO_WAIT 0

static inline void k_work_init_delayable(struct k_work_delayable *work,
					 void (*handler)(struct k_work *))
{
	(void)work;
	(void)handler;
}

static inline int k_work_reschedule(struct k_work_delayable *work, int timeout)
{
	(void)work;
	(void)timeout;
	return 0;
}

#endif /* LINKR_DEBUGGER_PD_SHELL_TEST_KERNEL_H_ */
