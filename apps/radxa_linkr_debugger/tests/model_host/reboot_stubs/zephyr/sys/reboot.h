/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef LINKR_DEBUGGER_REBOOT_STUB_REBOOT_H_
#define LINKR_DEBUGGER_REBOOT_STUB_REBOOT_H_

#define SYS_REBOOT_WARM 0
#define SYS_REBOOT_COLD 1

void sys_reboot(int type);

#endif
