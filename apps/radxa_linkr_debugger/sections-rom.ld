/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_ROM(http_resource_desc_linkr_debugger_http_service,
			     Z_LINK_ITERABLE_SUBALIGN)

/* Hard image budget: the flash image area is 843736 bytes. Keep at least
 * 8192 bytes free, so the image must not exceed 835544 bytes (0xCBFD8).
 */
ASSERT(_flash_used <= 0xCBFD8,
       "flash image exceeds 835544 B: _flash_used must stay <= 0xCBFD8, reserving 8192 B of 843736 B")
