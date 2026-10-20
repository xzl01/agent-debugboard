/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef LINKR_DEBUGGER_REBOOT_STUB_ATOMIC_H_
#define LINKR_DEBUGGER_REBOOT_STUB_ATOMIC_H_

#include <stdbool.h>

typedef int atomic_t;
typedef int atomic_val_t;

#define ATOMIC_INIT(value) (value)

static inline atomic_val_t atomic_get(const atomic_t *target)
{
	return *target;
}

static inline void atomic_set(atomic_t *target, atomic_val_t value)
{
	*target = value;
}

static inline bool atomic_cas(atomic_t *target, atomic_val_t old_value,
			      atomic_val_t new_value)
{
	if (*target != old_value) {
		return false;
	}

	*target = new_value;
	return true;
}

#endif
