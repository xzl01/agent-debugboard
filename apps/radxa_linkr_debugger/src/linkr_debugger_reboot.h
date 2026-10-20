#ifndef RADXA_LINKR_DEBUGGER_REBOOT_H_
#define RADXA_LINKR_DEBUGGER_REBOOT_H_

#define LINKR_DEBUGGER_REBOOT_DELAY_MS 250U

/* Idempotent cold reboots. These never request the ROM USB boot path. */
int linkr_debugger_reboot_now(void);
/* Schedule the same reboot once, after LINKR_DEBUGGER_REBOOT_DELAY_MS. */
int linkr_debugger_reboot_schedule(void);

#endif
