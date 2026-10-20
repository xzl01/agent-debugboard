#ifndef LINKR_DEBUGGER_SERIAL_RUNTIME_STUB_ATOMIC_H_
#define LINKR_DEBUGGER_SERIAL_RUNTIME_STUB_ATOMIC_H_

#include <stdatomic.h>

typedef atomic_int atomic_t;

#define ATOMIC_INIT(value) ATOMIC_VAR_INIT(value)

static inline int atomic_get(const atomic_t *target)
{
	return atomic_load(target);
}

static inline void atomic_set(atomic_t *target, int value)
{
	atomic_store(target, value);
}

static inline bool atomic_cas(atomic_t *target, int old_value, int new_value)
{
	return atomic_compare_exchange_strong(target, &old_value, new_value);
}

#endif
