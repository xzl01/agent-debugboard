/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_POWER_DEPENDENCY_H_
#define RADXA_LINKR_DEBUGGER_POWER_DEPENDENCY_H_

#include <stdbool.h>
#include <stddef.h>
#include <errno.h>

#define LINKR_DEBUGGER_POWER_DEPENDENCY_MAX_CHILDREN 32U

enum linkr_debugger_power_dependency_target {
	LINKR_DEBUGGER_POWER_DEPENDENCY_INDEPENDENT = 0,
	LINKR_DEBUGGER_POWER_DEPENDENCY_PARENT,
	LINKR_DEBUGGER_POWER_DEPENDENCY_CHILD,
};

/* Dependency ops for a parent rail with zero or more dependent children.
 * `child == false` addresses the parent (or the rail itself for independent
 * rails, index ignored); `child == true` addresses dependent child `index`
 * in up order: child 0 is enabled first and released last.
 */
struct linkr_debugger_power_dependency_ops {
	size_t (*child_count)(void *context);
	bool (*is_enabled)(void *context, bool child, size_t index);
	int (*set_enabled)(void *context, bool child, size_t index, bool enabled);
};

/* Apply `enabled` to the dependency tree.
 *
 * - INDEPENDENT: sets the rail itself.
 * - CHILD on (child `index`): enables the parent first when needed, then the
 *   child. If the child set fails after the parent was enabled here and no
 *   other child remains enabled, the parent enable is rolled back. Child off
 *   only releases that child; the parent is untouched.
 * - PARENT off: releases every enabled child in reverse child order (last
 *   child first), then the parent. If any step fails, already-released
 *   children are re-enabled in up order and the parent is left as-is, so a
 *   partial cascade never leaves the parent on with a half-released tree.
 * - PARENT on: only asserts the parent; re-qualification of children (for
 *   example the port-VBUS backfeed policy) is the caller's responsibility,
 *   so this path deliberately does not early-return before the caller can
 *   apply that policy.
 */
static inline int linkr_debugger_power_dependency_apply(
	enum linkr_debugger_power_dependency_target target, bool enabled,
	size_t index, const struct linkr_debugger_power_dependency_ops *ops,
	void *context)
{
	bool prerequisite_changed = false;
	size_t children = 0U;
	uint32_t released_mask = 0U;
	int ret;

	if (ops == NULL || ops->is_enabled == NULL || ops->set_enabled == NULL) {
		return -EINVAL;
	}
	if (ops->child_count != NULL) {
		children = ops->child_count(context);
	}
	if (children > LINKR_DEBUGGER_POWER_DEPENDENCY_MAX_CHILDREN) {
		return -E2BIG;
	}

	if (target == LINKR_DEBUGGER_POWER_DEPENDENCY_INDEPENDENT) {
		return ops->set_enabled(context, false, 0U, enabled);
	}

	if (target == LINKR_DEBUGGER_POWER_DEPENDENCY_CHILD) {
		if (index >= children) {
			return -EINVAL;
		}
		if (!enabled) {
			return ops->set_enabled(context, true, index, false);
		}
		if (!ops->is_enabled(context, false, 0U)) {
			ret = ops->set_enabled(context, false, 0U, true);
			if (ret < 0) {
				return ret;
			}
			prerequisite_changed = true;
		}
		ret = ops->set_enabled(context, true, index, true);
		if (ret < 0 && prerequisite_changed) {
			bool other_child_enabled = false;

			for (size_t i = 0U; i < children; i++) {
				if (i != index && ops->is_enabled(context, true, i)) {
					other_child_enabled = true;
					break;
				}
			}
			if (!other_child_enabled) {
				(void)ops->set_enabled(context, false, 0U, false);
			}
		}
		return ret;
	}

	/* target == LINKR_DEBUGGER_POWER_DEPENDENCY_PARENT */
	if (enabled) {
		return ops->set_enabled(context, false, 0U, true);
	}

	for (size_t i = children; i > 0U; i--) {
		const size_t child = i - 1U;

		if (!ops->is_enabled(context, true, child)) {
			continue;
		}
		ret = ops->set_enabled(context, true, child, false);
		if (ret < 0) {
			/* Roll back in up order and keep the parent enabled:
			 * never continue a cascade from a failed release.
			 */
			for (size_t j = 0U; j < children; j++) {
				if ((released_mask & (1UL << j)) != 0U) {
					(void)ops->set_enabled(context, true, j, true);
				}
			}
			return ret;
		}
		released_mask |= 1UL << child;
	}

	ret = ops->set_enabled(context, false, 0U, false);
	if (ret < 0) {
		for (size_t j = 0U; j < children; j++) {
			if ((released_mask & (1UL << j)) != 0U) {
				(void)ops->set_enabled(context, true, j, true);
			}
		}
	}
	return ret;
}

#endif /* RADXA_LINKR_DEBUGGER_POWER_DEPENDENCY_H_ */
