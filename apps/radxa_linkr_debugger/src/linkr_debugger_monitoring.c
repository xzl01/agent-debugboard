/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_monitoring.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/debug/cpu_load.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_SENSOR)
#include <zephyr/drivers/sensor.h>
#endif

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
#include <zephyr/sys/mem_stats.h>
#include <zephyr/sys/sys_heap.h>
#endif

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS) && defined(CONFIG_HEAP_MEM_POOL_SIZE) && \
	(CONFIG_HEAP_MEM_POOL_SIZE > 0)
extern struct k_heap _system_heap;
#endif

#if defined(CONFIG_SENSOR) && DT_HAS_CHOSEN(radxa_linkr_debugger_temperature_sensor)
#define LINKR_DEBUGGER_TEMPERATURE_NODE DT_CHOSEN(radxa_linkr_debugger_temperature_sensor)
static const struct device *const linkr_debugger_temperature_device =
	DEVICE_DT_GET(LINKR_DEBUGGER_TEMPERATURE_NODE);
#endif

static struct k_mutex linkr_debugger_monitoring_lock;
#if defined(CONFIG_CPU_LOAD)
static bool linkr_debugger_cpu_previous_valid;
static int64_t linkr_debugger_cpu_previous_uptime_ms;
#endif

static void linkr_debugger_monitoring_temperature_get(
	struct linkr_debugger_monitoring_temperature *temperature)
{
	temperature->available = false;
	temperature->reason = "no_zephyr_temperature_device";
	temperature->source = "";
	temperature->celsius_val1 = 0;
	temperature->celsius_val2 = 0;
	temperature->error = 0;

#if defined(CONFIG_SENSOR) && DT_HAS_CHOSEN(radxa_linkr_debugger_temperature_sensor)
	struct sensor_value value;
	int ret;

	if (!device_is_ready(linkr_debugger_temperature_device)) {
		temperature->reason = "device_not_ready";
		return;
	}

	ret = sensor_sample_fetch(linkr_debugger_temperature_device);
	if (ret < 0) {
		temperature->reason = "sample_fetch_failed";
		temperature->error = ret;
		return;
	}

	ret = sensor_channel_get(linkr_debugger_temperature_device, SENSOR_CHAN_DIE_TEMP, &value);
	if (ret < 0) {
		temperature->reason = ret == -ENOTSUP ? "temperature_channel_unsupported" :
					       "temperature_read_failed";
		temperature->error = ret;
		return;
	}

	temperature->available = true;
	temperature->reason = "";
	temperature->source = linkr_debugger_temperature_device->name;
	temperature->celsius_val1 = value.val1;
	temperature->celsius_val2 = value.val2;
#elif !defined(CONFIG_SENSOR)
	temperature->reason = "sensor_subsystem_disabled";
#endif
}

static void linkr_debugger_monitoring_heap_get(struct linkr_debugger_monitoring_heap *heap)
{
	heap->available = false;
	heap->reason = "runtime_stats_disabled";
	heap->source = "";
	heap->free_bytes = 0U;
	heap->allocated_bytes = 0U;
	heap->max_allocated_bytes = 0U;
	heap->total_bytes = 0U;
	heap->error = 0;

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS) && defined(CONFIG_HEAP_MEM_POOL_SIZE) && \
	(CONFIG_HEAP_MEM_POOL_SIZE > 0)
	struct sys_memory_stats stats;
	int ret;

	ret = sys_heap_runtime_stats_get(&_system_heap.heap, &stats);
	if (ret < 0) {
		heap->reason = "heap_stats_read_failed";
		heap->error = ret;
		return;
	}

	heap->available = true;
	heap->reason = "";
	heap->source = "system_heap";
	heap->free_bytes = stats.free_bytes;
	heap->allocated_bytes = stats.allocated_bytes;
	heap->max_allocated_bytes = stats.max_allocated_bytes;
	heap->total_bytes = stats.free_bytes + stats.allocated_bytes;
#elif defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
	heap->reason = "system_heap_unavailable";
#endif
}

static void linkr_debugger_monitoring_runtime_get(struct linkr_debugger_monitoring_runtime *runtime)
{
	runtime->available = false;
	runtime->reason = "uptime_unavailable";
	runtime->uptime_ms = 0;
	runtime->uptime_seconds = 0U;
	runtime->error = 0;

	runtime->available = true;
	runtime->reason = "";
	runtime->uptime_ms = k_uptime_get();
	runtime->uptime_seconds = (uint64_t)(runtime->uptime_ms / 1000);
}

static void linkr_debugger_monitoring_cpu_get(struct linkr_debugger_monitoring_cpu *cpu)
{
	cpu->available = false;
	cpu->reason = "thread_runtime_stats_disabled";
	cpu->active_pct_x100 = 0U;
	cpu->window_ms = 0U;
	cpu->busy_cycles_delta = 0U;
	cpu->total_cycles_delta = 0U;
	cpu->error = 0;

	uint32_t load_per_mille;
	int64_t uptime_ms;
	uint32_t window_ms;

#if defined(CONFIG_CPU_LOAD)
	load_per_mille = cpu_load_get(true);

	uptime_ms = k_uptime_get();
	if (!linkr_debugger_cpu_previous_valid) {
		linkr_debugger_cpu_previous_valid = true;
		linkr_debugger_cpu_previous_uptime_ms = uptime_ms;
		cpu->reason = "insufficient_runtime_window";
		return;
	}
	window_ms = uptime_ms > linkr_debugger_cpu_previous_uptime_ms ?
		(uint32_t)(uptime_ms - linkr_debugger_cpu_previous_uptime_ms) : 0U;
	linkr_debugger_cpu_previous_uptime_ms = uptime_ms;
	if (window_ms == 0U) {
		cpu->reason = "insufficient_runtime_window";
		return;
	}

	cpu->available = true;
	cpu->reason = "";
	cpu->busy_cycles_delta = 0U;
	cpu->total_cycles_delta = 0U;
	cpu->active_pct_x100 = load_per_mille * 10U;
	cpu->window_ms = window_ms;
#else
	cpu->reason = "cpu_load_disabled";
#endif
}

void linkr_debugger_monitoring_diagnostics_get(struct linkr_debugger_monitoring_diagnostics *diagnostics)
{
	if (diagnostics == NULL) {
		return;
	}

	memset(diagnostics, 0, sizeof(*diagnostics));
	linkr_debugger_monitoring_heap_get(&diagnostics->heap);
	linkr_debugger_monitoring_runtime_get(&diagnostics->runtime);
}

void linkr_debugger_monitoring_snapshot_get(struct linkr_debugger_monitoring_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return;
	}

	memset(snapshot, 0, sizeof(*snapshot));
	linkr_debugger_monitoring_temperature_get(&snapshot->temperature);
	linkr_debugger_monitoring_heap_get(&snapshot->heap);
	linkr_debugger_monitoring_runtime_get(&snapshot->runtime);

	k_mutex_lock(&linkr_debugger_monitoring_lock, K_FOREVER);
	linkr_debugger_monitoring_cpu_get(&snapshot->cpu);
	k_mutex_unlock(&linkr_debugger_monitoring_lock);
}

static int linkr_debugger_monitoring_init(void)
{
	k_mutex_init(&linkr_debugger_monitoring_lock);
	return 0;
}

SYS_INIT(linkr_debugger_monitoring_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
