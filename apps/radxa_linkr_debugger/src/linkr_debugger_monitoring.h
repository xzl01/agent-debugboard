/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_MONITORING_H_
#define RADXA_LINKR_DEBUGGER_MONITORING_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct linkr_debugger_monitoring_temperature {
	bool available;
	const char *reason;
	const char *source;
	int32_t celsius_val1;
	int32_t celsius_val2;
	int error;
};

struct linkr_debugger_monitoring_heap {
	bool available;
	const char *reason;
	const char *source;
	size_t free_bytes;
	size_t allocated_bytes;
	size_t max_allocated_bytes;
	size_t total_bytes;
	int error;
};

#define LINKR_DEBUGGER_MONITORING_THREAD_NAME_BUFSZ 32U

struct linkr_debugger_monitoring_memory_physical {
	size_t total_bytes;
	size_t image_reserved_bytes;
	uint32_t reserved_pct_x100;
};

struct linkr_debugger_monitoring_memory_stacks {
	size_t thread_count;
	size_t measured_count;
	size_t error_count;
	size_t total_bytes;
	size_t used_high_water_bytes;
	uint32_t max_pressure_pct_x100;
	char max_pressure_thread[LINKR_DEBUGGER_MONITORING_THREAD_NAME_BUFSZ];
};

struct linkr_debugger_monitoring_memory_pressure {
	bool available;
	const char *reason;
	const char *coverage;
	uint32_t pressure_pct_x100;
	const char *limiting_component;
	const char *limiting_name;
	uint32_t tie_count;
};

struct linkr_debugger_monitoring_memory {
	bool available;
	const char *reason;
	const char *source;
	const char *coverage;
	uint32_t pressure_pct_x100;
	const char *limiting_component;
	const char *limiting_name;
	uint32_t system_heap_pressure_pct_x100;
	struct linkr_debugger_monitoring_memory_physical physical;
	struct linkr_debugger_monitoring_memory_stacks stacks;
	struct linkr_debugger_monitoring_memory_pressure current_pressure;
	struct linkr_debugger_monitoring_memory_pressure peak_pressure;
};

struct linkr_debugger_monitoring_runtime {
	bool available;
	const char *reason;
	int64_t uptime_ms;
	uint64_t uptime_seconds;
	int error;
};

struct linkr_debugger_monitoring_cpu {
	bool available;
	const char *reason;
	uint32_t active_pct_x100;
	uint32_t window_ms;
	uint64_t busy_cycles_delta;
	uint64_t total_cycles_delta;
	int error;
};

struct linkr_debugger_monitoring_snapshot {
	struct linkr_debugger_monitoring_temperature temperature;
	struct linkr_debugger_monitoring_heap heap;
	struct linkr_debugger_monitoring_memory memory;
	struct linkr_debugger_monitoring_runtime runtime;
	struct linkr_debugger_monitoring_cpu cpu;
};

struct linkr_debugger_monitoring_diagnostics {
	struct linkr_debugger_monitoring_heap heap;
	struct linkr_debugger_monitoring_runtime runtime;
};

void linkr_debugger_monitoring_diagnostics_get(struct linkr_debugger_monitoring_diagnostics *diagnostics);
void linkr_debugger_monitoring_snapshot_get(struct linkr_debugger_monitoring_snapshot *snapshot);

#endif /* RADXA_LINKR_DEBUGGER_MONITORING_H_ */
