/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_ROM(http_resource_desc_linkr_debugger_http_service,
			     Z_LINK_ITERABLE_SUBALIGN)

/* Hard image budget: the flash image area is 847832 bytes. Keep at least
 * 8192 bytes free, so the image must not exceed 839640 bytes (0xCCFD8).
 */
ASSERT(_flash_used <= 0xCCFD8,
       "flash image exceeds 839640 B: _flash_used must stay <= 0xCCFD8, reserving 8192 B of 847832 B")
