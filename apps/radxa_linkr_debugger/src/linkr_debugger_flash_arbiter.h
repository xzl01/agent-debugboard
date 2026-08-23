#ifndef RADXA_LINKR_DEBUGGER_FLASH_ARBITER_H_
#define RADXA_LINKR_DEBUGGER_FLASH_ARBITER_H_

#include <stdbool.h>

enum linkr_debugger_flash_owner {
	LINKR_DEBUGGER_FLASH_OWNER_NONE = 0,
	LINKR_DEBUGGER_FLASH_OWNER_CONFIG,
	LINKR_DEBUGGER_FLASH_OWNER_OTA,
	LINKR_DEBUGGER_FLASH_OWNER_TASK,
};

void linkr_debugger_flash_arbiter_reset(void);
bool linkr_debugger_flash_arbiter_try_acquire(enum linkr_debugger_flash_owner owner);
bool linkr_debugger_flash_arbiter_release(enum linkr_debugger_flash_owner owner);
enum linkr_debugger_flash_owner linkr_debugger_flash_arbiter_owner(void);

#endif
