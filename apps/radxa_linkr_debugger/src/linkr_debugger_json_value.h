/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_JSON_VALUE_H_
#define RADXA_LINKR_DEBUGGER_JSON_VALUE_H_

#include <stdbool.h>
#include <stddef.h>

bool linkr_debugger_json_value_valid(const char *text, size_t max_depth,
				     size_t string_capacity);

#endif
