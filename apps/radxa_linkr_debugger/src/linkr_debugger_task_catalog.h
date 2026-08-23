/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_TASK_CATALOG_H_
#define RADXA_LINKR_DEBUGGER_TASK_CATALOG_H_

#include <stddef.h>
#include <stdint.h>

#define LINKR_DEBUGGER_TASK_CATALOG_HTTP_PATH "/api/v1/tasks/catalog"
#define LINKR_DEBUGGER_TASK_CATALOG_VERSION 1U
#define LINKR_DEBUGGER_TASK_CATALOG_TASK_COUNT 6U

const uint8_t *linkr_debugger_task_catalog_json(size_t *body_len);

#endif
