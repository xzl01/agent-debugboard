/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_POWER_DEPENDENCY_H_
#define RADXA_LINKR_DEBUGGER_POWER_DEPENDENCY_H_

#include <stdbool.h>
#include <errno.h>

enum linkr_debugger_power_dependency_target {
	LINKR_DEBUGGER_POWER_DEPENDENCY_INDEPENDENT = 0,
	LINKR_DEBUGGER_POWER_DEPENDENCY_PARENT,
	LINKR_DEBUGGER_POWER_DEPENDENCY_CHILD,
};

struct linkr_debugger_power_dependency_ops {
	bool (*is_enabled)(void *context, bool child);
	int (*set_enabled)(void *context, bool child, bool enabled);
};

static inline int linkr_debugger_power_dependency_apply(
	enum linkr_debugger_power_dependency_target target, bool enabled,
	const struct linkr_debugger_power_dependency_ops *ops, void *context)
{
	bool prerequisite_changed = false;
	int ret;

	if (ops == NULL || ops->is_enabled == NULL || ops->set_enabled == NULL) {
		return -EINVAL;
	}

	if (target == LINKR_DEBUGGER_POWER_DEPENDENCY_INDEPENDENT) {
		return ops->set_enabled(context, false, enabled);
	}

	if (target == LINKR_DEBUGGER_POWER_DEPENDENCY_CHILD) {
		if (!enabled) {
			return ops->set_enabled(context, true, false);
		}
		if (!ops->is_enabled(context, false)) {
			ret = ops->set_enabled(context, false, true);
			if (ret < 0) {
				return ret;
			}
			prerequisite_changed = true;
		}
		ret = ops->set_enabled(context, true, true);
		if (ret < 0 && prerequisite_changed) {
			(void)ops->set_enabled(context, false, false);
		}
		return ret;
	}

	if (enabled) {
		return ops->set_enabled(context, false, true);
	}
	if (ops->is_enabled(context, true)) {
		ret = ops->set_enabled(context, true, false);
		if (ret < 0) {
			return ret;
		}
		prerequisite_changed = true;
	}
	ret = ops->set_enabled(context, false, false);
	if (ret < 0 && prerequisite_changed) {
		(void)ops->set_enabled(context, true, true);
	}
	return ret;
}

#endif /* RADXA_LINKR_DEBUGGER_POWER_DEPENDENCY_H_ */
