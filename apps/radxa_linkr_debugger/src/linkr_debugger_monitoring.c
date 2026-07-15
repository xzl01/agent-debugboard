/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_monitoring.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/debug/cpu_load.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/linker/linker-defs.h>
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

static uint32_t linkr_debugger_monitoring_pct_x100(size_t used, size_t total)
{
	uint64_t scaled;

	if (total == 0U) {
		return 0U;
	}
	if (used >= total) {
		return 10000U;
	}
	if ((uint64_t)used > UINT64_MAX / 10000ULL) {
		return 10000U;
	}

	scaled = (uint64_t)used * 10000ULL;
	return (uint32_t)(scaled / (uint64_t)total);
}

static size_t linkr_debugger_monitoring_size_add(size_t a, size_t b)
{
	if (SIZE_MAX - a < b) {
		return SIZE_MAX;
	}

	return a + b;
}

static const char *linkr_debugger_monitoring_pressure_coverage(bool heap_available,
							       bool network_available,
							       bool stacks_available)
{
	if (heap_available && network_available && stacks_available) {
		return "system_heap_network_and_stacks";
	}
	if (heap_available && network_available) {
		return "system_heap_and_network";
	}
	if (heap_available && stacks_available) {
		return "system_heap_and_stacks";
	}
	if (network_available && stacks_available) {
		return "network_and_stacks";
	}
	if (heap_available) {
		return "system_heap_only";
	}
	if (network_available) {
		return "network_only";
	}
	if (stacks_available) {
		return "stacks_only";
	}

	return "none";
}

static void linkr_debugger_monitoring_pressure_init(
	struct linkr_debugger_monitoring_memory_pressure *pressure, const char *coverage)
{
	pressure->available = false;
	pressure->reason = "pressure_sources_unavailable";
	pressure->coverage = coverage;
	pressure->pressure_pct_x100 = 0U;
	pressure->limiting_component = "none";
	pressure->limiting_name = "";
	pressure->tie_count = 0U;
}

static void linkr_debugger_monitoring_pressure_consider(
	struct linkr_debugger_monitoring_memory_pressure *pressure, const char *component,
	const char *name, uint32_t pressure_pct_x100)
{
	if (!pressure->available) {
		pressure->available = true;
		pressure->reason = "";
		pressure->pressure_pct_x100 = pressure_pct_x100;
		pressure->limiting_component = component;
		pressure->limiting_name = name;
		pressure->tie_count = 1U;
		return;
	}

	if (pressure_pct_x100 > pressure->pressure_pct_x100) {
		pressure->pressure_pct_x100 = pressure_pct_x100;
		pressure->limiting_component = component;
		pressure->limiting_name = name;
		pressure->tie_count = 1U;
	} else if (pressure_pct_x100 == pressure->pressure_pct_x100) {
		pressure->tie_count++;
	}
}

static void linkr_debugger_monitoring_copy_thread_name(const struct k_thread *thread,
						      char *buf, size_t buf_size)
{
	const char *name = NULL;

	if (buf == NULL || buf_size == 0U) {
		return;
	}

	buf[0] = '\0';

#if defined(CONFIG_THREAD_NAME)
	name = k_thread_name_get((k_tid_t)thread);
#else
	ARG_UNUSED(thread);
#endif
	if (name == NULL || name[0] == '\0') {
		name = "unnamed";
	}

	(void)snprintk(buf, buf_size, "%s", name);
}

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

struct linkr_debugger_monitoring_stack_walk {
	struct linkr_debugger_monitoring_memory_stacks *stacks;
};

#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO) && \
	defined(CONFIG_THREAD_MONITOR)
static void linkr_debugger_monitoring_stack_cb(const struct k_thread *thread, void *user_data)
{
	struct linkr_debugger_monitoring_stack_walk *walk = user_data;
	struct linkr_debugger_monitoring_memory_stacks *stacks = walk->stacks;
	size_t unused_bytes = 0U;
	size_t stack_size;
	size_t used_bytes;
	uint32_t pressure_pct_x100;
	int ret;

	stacks->thread_count++;
	stack_size = thread->stack_info.size;
	if (stack_size == 0U) {
		stacks->error_count++;
		return;
	}

	ret = k_thread_stack_space_get(thread, &unused_bytes);
	if (ret < 0 || unused_bytes > stack_size) {
		stacks->error_count++;
		return;
	}

	used_bytes = stack_size - unused_bytes;
	pressure_pct_x100 = linkr_debugger_monitoring_pct_x100(used_bytes, stack_size);
	stacks->measured_count++;
	stacks->total_bytes = linkr_debugger_monitoring_size_add(stacks->total_bytes, stack_size);
	stacks->used_high_water_bytes = linkr_debugger_monitoring_size_add(
		stacks->used_high_water_bytes, used_bytes);
	if (pressure_pct_x100 >= stacks->max_pressure_pct_x100) {
		stacks->max_pressure_pct_x100 = pressure_pct_x100;
		linkr_debugger_monitoring_copy_thread_name(thread, stacks->max_pressure_thread,
							      sizeof(stacks->max_pressure_thread));
	}
}
#endif

static void linkr_debugger_monitoring_memory_stacks_get(
	struct linkr_debugger_monitoring_memory_stacks *stacks)
{
	memset(stacks, 0, sizeof(*stacks));
	stacks->max_pressure_thread[0] = '\0';

#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO) && \
	defined(CONFIG_THREAD_MONITOR)
	struct linkr_debugger_monitoring_stack_walk walk = {
		.stacks = stacks,
	};

	k_thread_foreach(linkr_debugger_monitoring_stack_cb, &walk);
#endif
}

static bool linkr_debugger_monitoring_slab_pressure(
	struct k_mem_slab *slab, uint32_t *current_pct_x100, uint32_t *peak_pct_x100)
{
	uint32_t used;
	uint32_t free;
	uint32_t total;

	if (slab == NULL) {
		return false;
	}

	used = k_mem_slab_num_used_get(slab);
	free = k_mem_slab_num_free_get(slab);
	total = used + free;
	if (total == 0U) {
		return false;
	}

	*current_pct_x100 = linkr_debugger_monitoring_pct_x100(used, total);
	/* k_mem_slab_max_used_get() is a boot-lifetime high-water mark with tracing enabled. */
	*peak_pct_x100 = linkr_debugger_monitoring_pct_x100(k_mem_slab_max_used_get(slab), total);
	return true;
}

static bool linkr_debugger_monitoring_net_buf_pressure(
	struct net_buf_pool *pool, uint32_t *current_pct_x100, uint32_t *peak_pct_x100)
{
	size_t total;
	size_t available;
	size_t used;

	if (pool == NULL || pool->buf_count == 0U) {
		return false;
	}

	total = pool->buf_count;
	available = net_buf_get_available(pool);
	used = available < total ? total - available : 0U;
	*current_pct_x100 = linkr_debugger_monitoring_pct_x100(used, total);
	/* net_buf_get_max_used() is a boot-lifetime high-water mark with pool usage enabled. */
	*peak_pct_x100 = linkr_debugger_monitoring_pct_x100(net_buf_get_max_used(pool), total);
	return true;
}

static void linkr_debugger_monitoring_memory_pressure_get(
	struct linkr_debugger_monitoring_memory *memory,
	const struct linkr_debugger_monitoring_heap *heap)
{
	struct k_mem_slab *rx_pkt = NULL;
	struct k_mem_slab *tx_pkt = NULL;
	struct net_buf_pool *rx_data = NULL;
	struct net_buf_pool *tx_data = NULL;
	uint32_t current_pct_x100;
	uint32_t peak_pct_x100;
	bool heap_available = heap != NULL && heap->available && heap->total_bytes > 0U;
	bool network_available = false;
	bool stacks_available = memory->stacks.measured_count > 0U;

	net_pkt_get_info(&rx_pkt, &tx_pkt, &rx_data, &tx_data);

	network_available = linkr_debugger_monitoring_slab_pressure(rx_pkt, &current_pct_x100,
							      &peak_pct_x100) || network_available;
	network_available = linkr_debugger_monitoring_slab_pressure(tx_pkt, &current_pct_x100,
							      &peak_pct_x100) || network_available;
	network_available = linkr_debugger_monitoring_net_buf_pressure(rx_data, &current_pct_x100,
								   &peak_pct_x100) || network_available;
	network_available = linkr_debugger_monitoring_net_buf_pressure(tx_data, &current_pct_x100,
								   &peak_pct_x100) || network_available;

	linkr_debugger_monitoring_pressure_init(&memory->current_pressure,
		linkr_debugger_monitoring_pressure_coverage(heap_available, network_available, false));
	linkr_debugger_monitoring_pressure_init(&memory->peak_pressure,
		linkr_debugger_monitoring_pressure_coverage(heap_available, network_available,
									 stacks_available));

	if (heap_available) {
		current_pct_x100 = linkr_debugger_monitoring_pct_x100(heap->allocated_bytes,
									 heap->total_bytes);
		peak_pct_x100 = linkr_debugger_monitoring_pct_x100(heap->max_allocated_bytes,
								       heap->total_bytes);
		linkr_debugger_monitoring_pressure_consider(&memory->current_pressure,
			"system_heap", "system_heap", current_pct_x100);
		linkr_debugger_monitoring_pressure_consider(&memory->peak_pressure,
			"system_heap", "system_heap", peak_pct_x100);
	}

	if (linkr_debugger_monitoring_slab_pressure(rx_pkt, &current_pct_x100, &peak_pct_x100)) {
		linkr_debugger_monitoring_pressure_consider(&memory->current_pressure,
			"net_pkt_rx", "net_pkt_rx", current_pct_x100);
		linkr_debugger_monitoring_pressure_consider(&memory->peak_pressure,
			"net_pkt_rx", "net_pkt_rx", peak_pct_x100);
	}
	if (linkr_debugger_monitoring_slab_pressure(tx_pkt, &current_pct_x100, &peak_pct_x100)) {
		linkr_debugger_monitoring_pressure_consider(&memory->current_pressure,
			"net_pkt_tx", "net_pkt_tx", current_pct_x100);
		linkr_debugger_monitoring_pressure_consider(&memory->peak_pressure,
			"net_pkt_tx", "net_pkt_tx", peak_pct_x100);
	}
	if (linkr_debugger_monitoring_net_buf_pressure(rx_data, &current_pct_x100, &peak_pct_x100)) {
		linkr_debugger_monitoring_pressure_consider(&memory->current_pressure,
			"net_buf_rx_data", "net_buf_rx_data", current_pct_x100);
		linkr_debugger_monitoring_pressure_consider(&memory->peak_pressure,
			"net_buf_rx_data", "net_buf_rx_data", peak_pct_x100);
	}
	if (linkr_debugger_monitoring_net_buf_pressure(tx_data, &current_pct_x100, &peak_pct_x100)) {
		linkr_debugger_monitoring_pressure_consider(&memory->current_pressure,
			"net_buf_tx_data", "net_buf_tx_data", current_pct_x100);
		linkr_debugger_monitoring_pressure_consider(&memory->peak_pressure,
			"net_buf_tx_data", "net_buf_tx_data", peak_pct_x100);
	}
	if (stacks_available) {
		linkr_debugger_monitoring_pressure_consider(&memory->peak_pressure,
			"thread_stack", memory->stacks.max_pressure_thread,
			memory->stacks.max_pressure_pct_x100);
	}
}

static void linkr_debugger_monitoring_memory_get(
	struct linkr_debugger_monitoring_memory *memory,
	const struct linkr_debugger_monitoring_heap *heap)
{
	bool physical_available = false;
	bool heap_available;
	bool stacks_available;

	memory->available = false;
	memory->reason = "pressure_sources_unavailable";
	memory->source = "zephyr";
	memory->coverage = "none";
	memory->pressure_pct_x100 = 0U;
	memory->limiting_component = "none";
	memory->limiting_name = "";
	memory->system_heap_pressure_pct_x100 = 0U;
	memset(&memory->physical, 0, sizeof(memory->physical));
	memset(&memory->stacks, 0, sizeof(memory->stacks));
	linkr_debugger_monitoring_pressure_init(&memory->current_pressure, "none");
	linkr_debugger_monitoring_pressure_init(&memory->peak_pressure, "none");

#if defined(CONFIG_SRAM_SIZE) && (CONFIG_SRAM_SIZE > 0)
	uintptr_t ram_start = (uintptr_t)_image_ram_start;
	uintptr_t ram_end = (uintptr_t)_image_ram_end;

	memory->physical.total_bytes = (size_t)CONFIG_SRAM_SIZE * 1024U;
	if (ram_end >= ram_start) {
		memory->physical.image_reserved_bytes = (size_t)(ram_end - ram_start);
		memory->physical.reserved_pct_x100 = linkr_debugger_monitoring_pct_x100(
			memory->physical.image_reserved_bytes, memory->physical.total_bytes);
		physical_available = true;
	}
#endif

	if (heap != NULL && heap->available && heap->total_bytes > 0U) {
		memory->system_heap_pressure_pct_x100 = linkr_debugger_monitoring_pct_x100(
			heap->allocated_bytes, heap->total_bytes);
	}

	linkr_debugger_monitoring_memory_stacks_get(&memory->stacks);
	linkr_debugger_monitoring_memory_pressure_get(memory, heap);

	heap_available = heap != NULL && heap->available && heap->total_bytes > 0U;
	stacks_available = memory->stacks.measured_count > 0U;
	if (!heap_available && !stacks_available) {
		if (physical_available) {
			memory->coverage = "physical_only";
		}
		return;
	}

	memory->available = true;
	memory->reason = "";
	if (heap_available && stacks_available) {
		memory->coverage = "heap_and_stacks";
	} else if (heap_available) {
		memory->coverage = "heap_only";
	} else if (stacks_available) {
		memory->coverage = "stacks_only";
	}

	if (heap_available) {
		memory->pressure_pct_x100 = memory->system_heap_pressure_pct_x100;
		memory->limiting_component = "system_heap";
		memory->limiting_name = "system_heap";
	}
	if (stacks_available && memory->stacks.max_pressure_pct_x100 >= memory->pressure_pct_x100) {
		memory->pressure_pct_x100 = memory->stacks.max_pressure_pct_x100;
		memory->limiting_component = "thread_stack";
		memory->limiting_name = memory->stacks.max_pressure_thread;
	}
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
	linkr_debugger_monitoring_memory_get(&snapshot->memory, &snapshot->heap);
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
