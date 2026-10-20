/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef LINKR_PD_TEST_KERNEL_H_
#define LINKR_PD_TEST_KERNEL_H_

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

struct k_mutex {
	bool locked;
};

extern int64_t linkr_pd_test_uptime_ms;

#define K_MUTEX_DEFINE(name) struct k_mutex name
#define K_FOREVER 0

static inline void k_mutex_init(struct k_mutex *lock)
{
	lock->locked = false;
}

static inline int k_mutex_lock(struct k_mutex *lock, int timeout)
{
	(void)timeout;
	assert(!lock->locked);
	lock->locked = true;
	return 0;
}

static inline int k_mutex_unlock(struct k_mutex *lock)
{
	assert(lock->locked);
	lock->locked = false;
	return 0;
}

static inline int64_t k_uptime_get(void)
{
	return linkr_pd_test_uptime_ms;
}

static inline void k_msleep(int duration_ms)
{
	linkr_pd_test_uptime_ms += duration_ms;
}

#endif
