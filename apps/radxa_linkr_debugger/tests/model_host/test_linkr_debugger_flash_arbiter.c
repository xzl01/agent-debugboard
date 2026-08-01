#include "linkr_debugger_flash_arbiter.h"

#include <assert.h>

static void test_initial_owner(void)
{
	linkr_debugger_flash_arbiter_reset();
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_NONE);
	assert(!linkr_debugger_flash_arbiter_try_acquire(LINKR_DEBUGGER_FLASH_OWNER_NONE));
	assert(!linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_NONE));
}

static void assert_owner_excludes(enum linkr_debugger_flash_owner owner,
				  enum linkr_debugger_flash_owner excluded)
{
	linkr_debugger_flash_arbiter_reset();
	assert(linkr_debugger_flash_arbiter_try_acquire(owner));
	assert(!linkr_debugger_flash_arbiter_try_acquire(owner));
	assert(!linkr_debugger_flash_arbiter_try_acquire(excluded));
	assert(!linkr_debugger_flash_arbiter_release(excluded));
	assert(linkr_debugger_flash_arbiter_owner() == owner);
	assert(linkr_debugger_flash_arbiter_release(owner));
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_NONE);
}

static void test_config_ota_exclusion(void)
{
	assert_owner_excludes(LINKR_DEBUGGER_FLASH_OWNER_CONFIG,
			      LINKR_DEBUGGER_FLASH_OWNER_OTA);
	assert_owner_excludes(LINKR_DEBUGGER_FLASH_OWNER_OTA,
			      LINKR_DEBUGGER_FLASH_OWNER_CONFIG);
}

int main(void)
{
	test_initial_owner();
	test_config_ota_exclusion();
	return 0;
}
