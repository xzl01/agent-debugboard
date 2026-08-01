#include "linkr_debugger_flash_arbiter.h"

#ifdef LINKR_DEBUGGER_FLASH_ARBITER_HOST_TEST
#include <stdatomic.h>
#else
#include <zephyr/irq.h>
#endif

#ifdef LINKR_DEBUGGER_FLASH_ARBITER_HOST_TEST

static atomic_int linkr_debugger_flash_arbiter_current_owner =
	ATOMIC_VAR_INIT(LINKR_DEBUGGER_FLASH_OWNER_NONE);

static void linkr_debugger_flash_arbiter_store(enum linkr_debugger_flash_owner owner)
{
	atomic_store_explicit(&linkr_debugger_flash_arbiter_current_owner, owner,
			      memory_order_release);
}

static bool linkr_debugger_flash_arbiter_replace(enum linkr_debugger_flash_owner expected,
					 enum linkr_debugger_flash_owner desired)
{
	int expected_value = expected;

	return atomic_compare_exchange_strong_explicit(
		&linkr_debugger_flash_arbiter_current_owner, &expected_value, desired,
		memory_order_acq_rel, memory_order_acquire);
}

static enum linkr_debugger_flash_owner linkr_debugger_flash_arbiter_load(void)
{
	return (enum linkr_debugger_flash_owner)atomic_load_explicit(
		&linkr_debugger_flash_arbiter_current_owner, memory_order_acquire);
}

#else

static enum linkr_debugger_flash_owner linkr_debugger_flash_arbiter_current_owner =
	LINKR_DEBUGGER_FLASH_OWNER_NONE;

static void linkr_debugger_flash_arbiter_store(enum linkr_debugger_flash_owner owner)
{
	unsigned int key = irq_lock();

	linkr_debugger_flash_arbiter_current_owner = owner;
	irq_unlock(key);
}

static bool linkr_debugger_flash_arbiter_replace(enum linkr_debugger_flash_owner expected,
					 enum linkr_debugger_flash_owner desired)
{
	bool replaced = false;
	unsigned int key = irq_lock();

	if (linkr_debugger_flash_arbiter_current_owner == expected) {
		linkr_debugger_flash_arbiter_current_owner = desired;
		replaced = true;
	}
	irq_unlock(key);
	return replaced;
}

static enum linkr_debugger_flash_owner linkr_debugger_flash_arbiter_load(void)
{
	enum linkr_debugger_flash_owner owner;
	unsigned int key = irq_lock();

	owner = linkr_debugger_flash_arbiter_current_owner;
	irq_unlock(key);
	return owner;
}

#endif

void linkr_debugger_flash_arbiter_reset(void)
{
	linkr_debugger_flash_arbiter_store(LINKR_DEBUGGER_FLASH_OWNER_NONE);
}

bool linkr_debugger_flash_arbiter_try_acquire(enum linkr_debugger_flash_owner owner)
{
	if (owner == LINKR_DEBUGGER_FLASH_OWNER_NONE) {
		return false;
	}

	return linkr_debugger_flash_arbiter_replace(LINKR_DEBUGGER_FLASH_OWNER_NONE, owner);
}

bool linkr_debugger_flash_arbiter_release(enum linkr_debugger_flash_owner owner)
{
	if (owner == LINKR_DEBUGGER_FLASH_OWNER_NONE) {
		return false;
	}

	return linkr_debugger_flash_arbiter_replace(owner, LINKR_DEBUGGER_FLASH_OWNER_NONE);
}

enum linkr_debugger_flash_owner linkr_debugger_flash_arbiter_owner(void)
{
	return linkr_debugger_flash_arbiter_load();
}
