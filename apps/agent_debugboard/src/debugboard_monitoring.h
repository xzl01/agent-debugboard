/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AGENT_DEBUGBOARD_MONITORING_H_
#define AGENT_DEBUGBOARD_MONITORING_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct debugboard_monitoring_temperature {
	bool available;
	const char *reason;
	const char *source;
	int32_t celsius_val1;
	int32_t celsius_val2;
	int error;
};

struct debugboard_monitoring_heap {
	bool available;
	const char *reason;
	const char *source;
	size_t free_bytes;
	size_t allocated_bytes;
	size_t max_allocated_bytes;
	size_t total_bytes;
	int error;
};

struct debugboard_monitoring_runtime {
	bool available;
	const char *reason;
	int64_t uptime_ms;
	uint64_t uptime_seconds;
	int error;
};

struct debugboard_monitoring_cpu {
	bool available;
	const char *reason;
	uint32_t active_pct_x100;
	uint32_t window_ms;
	uint64_t busy_cycles_delta;
	uint64_t total_cycles_delta;
	int error;
};

struct debugboard_monitoring_snapshot {
	struct debugboard_monitoring_temperature temperature;
	struct debugboard_monitoring_heap heap;
	struct debugboard_monitoring_runtime runtime;
	struct debugboard_monitoring_cpu cpu;
};

struct debugboard_monitoring_diagnostics {
	struct debugboard_monitoring_heap heap;
	struct debugboard_monitoring_runtime runtime;
};

void debugboard_monitoring_diagnostics_get(struct debugboard_monitoring_diagnostics *diagnostics);
void debugboard_monitoring_snapshot_get(struct debugboard_monitoring_snapshot *snapshot);

#endif /* AGENT_DEBUGBOARD_MONITORING_H_ */
