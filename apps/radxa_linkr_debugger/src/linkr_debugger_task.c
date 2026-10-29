/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 *
 * Atomic four-slot task-v2 store on the proven Settings NVS backend.
 * Non-overlapping keys:
 *   linkr/task/manifest           commit record; always written last
 *   linkr/task/ir/<slot>/<bank>   standalone one-task IR blob (A/B per slot)
 *   linkr/task/src/<slot>/<bank>  exact lossless source (A/B per slot)
 *
 * Every data key is written to the bank the current manifest does not
 * reference, read back and hashed before the single manifest write makes
 * the new state visible. A power cut therefore leaves either the complete
 * old state or the complete new state. Keys not referenced by the live
 * manifest are only deleted after that manifest write commits.
 */

#include "linkr_debugger_task.h"

#include "linkr_debugger_json_cursor.h"
#include "linkr_debugger_task_blob.h"
#include "linkr_debugger_task_catalog.h"
#include "linkr_debugger_task_sha256.h"
#include "linkr_debugger_task_store.h"

#ifndef LINKR_DEBUGGER_TASK_HOST_TEST
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#else
#include <pthread.h>
#endif

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef LINKR_DEBUGGER_TASK_HOST_TEST
LOG_MODULE_REGISTER(linkr_debugger_task, CONFIG_LINKR_DEBUGGER_LOG_LEVEL);
#endif

#define TASK_MANIFEST_MAGIC 0x324d5452U /* RTM2 */
#define TASK_MANIFEST_VERSION 3U
#define TASK_KEY_PREFIX "linkr/task/"
#define TASK_KEY_MANIFEST TASK_KEY_PREFIX "manifest"
#define TASK_KEY_IR_FMT TASK_KEY_PREFIX "ir/%u/%u"
#define TASK_KEY_SOURCE_FMT TASK_KEY_PREFIX "src/%u/%u"
#define TASK_KEY_NAME_SIZE 32U
#define TASK_STORAGE_KEY_OVERHEAD 64U
#define TASK_REAL_FLASH_SIZE 0x200000U
#define TASK_SETTINGS_OFFSET 0x1b0000U
#define TASK_SETTINGS_SIZE 0x20000U

struct task_manifest_slot {
	uint8_t present;
	uint8_t source_present;
	uint8_t source_bank;
	uint8_t ir_bank;
	uint8_t order;
	uint8_t reserved[3];
	uint16_t request_count;
	uint16_t source_len;
	uint16_t ir_len;
	uint16_t reserved2;
	char id[LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN + 1U];
	uint8_t source_digest[LINKR_DEBUGGER_TASK_SHA256_SIZE];
	uint8_t ir_digest[LINKR_DEBUGGER_TASK_SHA256_SIZE];
};

struct task_manifest {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint8_t store_id[LINKR_DEBUGGER_TASK_STORE_ID_SIZE];
	uint64_t revision;
	uint8_t catalog_fingerprint[LINKR_DEBUGGER_TASK_SHA256_SIZE];
	uint16_t full_ir_len;
	uint16_t reserved;
	uint8_t full_ir_digest[LINKR_DEBUGGER_TASK_SHA256_SIZE];
	uint8_t task_count;
	uint8_t reserved2[3];
	struct task_manifest_slot slots[LINKR_DEBUGGER_TASK_MAX_TASKS];
};

struct task_piece {
	const char *section;
	size_t section_len;
};

struct task_slot_write {
	bool ir_pending;
	bool source_pending;
	struct task_piece piece;
};

struct task_runtime_state {
	bool backend_available;
	struct task_manifest manifest;
	struct linkr_debugger_task_status status;
	struct linkr_debugger_task_blob_layout layout;
	char active_ir[LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE];
};

struct task_work_state {
	struct task_manifest manifest;
	struct linkr_debugger_task_blob_layout layout;
	char full_ir[LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE];
	size_t full_ir_len;
	struct task_piece pieces[LINKR_DEBUGGER_TASK_MAX_TASKS];
	struct task_slot_write writes[LINKR_DEBUGGER_TASK_MAX_TASKS];
	bool delete_source_keys[LINKR_DEBUGGER_TASK_MAX_TASKS];
	bool delete_slot_keys[LINKR_DEBUGGER_TASK_MAX_TASKS];
};

static struct task_runtime_state task_runtime;
static struct task_work_state task_work;
/* Standalone-IR staging, readback and source-load arena. */
static char task_stage[LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE];

_Static_assert(sizeof(struct task_manifest) < 1024U,
	       "task manifest must remain a small Settings record");

#ifdef LINKR_DEBUGGER_TASK_HOST_TEST

static pthread_mutex_t task_lock_object = PTHREAD_MUTEX_INITIALIZER;
#define TASK_LOCK() ((void)pthread_mutex_lock(&task_lock_object))
#define TASK_UNLOCK() ((void)pthread_mutex_unlock(&task_lock_object))

static const struct linkr_debugger_task_backend_ops *task_test_backend;
static void *task_test_backend_context;
static ssize_t task_test_storage_free = -1;

void linkr_debugger_task_test_set_backend(
	const struct linkr_debugger_task_backend_ops *ops, void *context)
{
	task_test_backend = ops;
	task_test_backend_context = context;
}

static int task_backend_subsys_init(void)
{
	return task_test_backend != NULL && task_test_backend->subsys_init != NULL ?
		task_test_backend->subsys_init(task_test_backend_context) : -ENODEV;
}

static int task_backend_load_one(const char *name, void *value, size_t value_size)
{
	return task_test_backend != NULL && task_test_backend->load_one != NULL ?
		task_test_backend->load_one(task_test_backend_context, name, value,
					   value_size) :
		-ENODEV;
}

static int task_backend_save_one(const char *name, const void *value, size_t value_size)
{
	return task_test_backend != NULL && task_test_backend->save_one != NULL ?
		task_test_backend->save_one(task_test_backend_context, name, value,
					   value_size) :
		-ENODEV;
}

static int task_backend_delete_one(const char *name)
{
	return task_test_backend != NULL && task_test_backend->delete_one != NULL ?
		task_test_backend->delete_one(task_test_backend_context, name) : -ENODEV;
}

static void task_backend_random(void *value, size_t value_size)
{
	if (task_test_backend != NULL && task_test_backend->random != NULL) {
		task_test_backend->random(task_test_backend_context, value, value_size);
	} else {
		memset(value, 0xa5, value_size);
	}
}

#else /* LINKR_DEBUGGER_TASK_HOST_TEST */

K_MUTEX_DEFINE(task_lock_object);
#define TASK_LOCK() ((void)k_mutex_lock(&task_lock_object, K_FOREVER))
#define TASK_UNLOCK() ((void)k_mutex_unlock(&task_lock_object))

#if !DT_NODE_EXISTS(DT_NODELABEL(storage_partition))
#error "storage_partition must exist for the task Settings namespace"
#else
#define TASK_FLASH_BASE DT_REG_ADDR(DT_NODELABEL(flash0))
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(storage_partition)) - TASK_FLASH_BASE ==
		     TASK_SETTINGS_OFFSET,
	     "Settings partition offset changed");
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(storage_partition)) == TASK_SETTINGS_SIZE,
	     "Settings partition must be 128KiB");
BUILD_ASSERT(TASK_SETTINGS_OFFSET + TASK_SETTINGS_SIZE <= TASK_REAL_FLASH_SIZE,
	     "Settings partition must stay inside the populated 2MiB flash");
#endif

static int task_backend_subsys_init(void)
{
	return settings_subsys_init();
}

static int task_backend_load_one(const char *name, void *value, size_t value_size)
{
	return (int)settings_load_one(name, value, value_size);
}

static int task_backend_save_one(const char *name, const void *value, size_t value_size)
{
	return settings_save_one(name, value, value_size);
}

static int task_backend_delete_one(const char *name)
{
	return settings_delete(name);
}

static void task_backend_random(void *value, size_t value_size)
{
	sys_rand_get(value, value_size);
}

#endif /* LINKR_DEBUGGER_TASK_HOST_TEST */

static void task_key_ir(char name[TASK_KEY_NAME_SIZE], uint8_t slot, uint8_t bank)
{
	(void)snprintf(name, TASK_KEY_NAME_SIZE, TASK_KEY_IR_FMT,
		       (unsigned int)slot, (unsigned int)bank);
}

static void task_key_source(char name[TASK_KEY_NAME_SIZE], uint8_t slot, uint8_t bank)
{
	(void)snprintf(name, TASK_KEY_NAME_SIZE, TASK_KEY_SOURCE_FMT,
		       (unsigned int)slot, (unsigned int)bank);
}

static void task_catalog_fingerprint(uint8_t digest[LINKR_DEBUGGER_TASK_SHA256_SIZE])
{
	size_t len;
	const uint8_t *catalog = linkr_debugger_task_catalog_json(&len);

	linkr_debugger_task_sha256_digest(catalog, len, digest);
}

static void task_status_reset(void)
{
	memset(&task_runtime, 0, sizeof(task_runtime));
}

static int task_find_manifest_slot(const struct task_manifest *manifest,
				   const char *id)
{
	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
		if (manifest->slots[i].present != 0U &&
		    strcmp(manifest->slots[i].id, id) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static int task_find_slot_by_order(const struct task_manifest *manifest,
				   uint8_t order)
{
	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
		if (manifest->slots[i].present != 0U &&
		    manifest->slots[i].order == order) {
			return (int)i;
		}
	}
	return -1;
}

static int task_find_free_slot(const struct task_manifest *manifest)
{
	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
		if (manifest->slots[i].present == 0U) {
			return (int)i;
		}
	}
	return -1;
}

static void task_digest_section(const char *ir,
				const struct linkr_debugger_task_blob_entry *entry,
				uint8_t digest[LINKR_DEBUGGER_TASK_SHA256_SIZE])
{
	struct linkr_debugger_task_sha256 context;

	linkr_debugger_task_sha256_init(&context);
	linkr_debugger_task_sha256_update(&context, LINKR_DEBUGGER_TASK_BLOB_HEADER,
					  LINKR_DEBUGGER_TASK_BLOB_HEADER_LEN);
	linkr_debugger_task_sha256_update(&context, ir + entry->section_start,
					  entry->digest_end - entry->section_start);
	linkr_debugger_task_sha256_final(&context, digest);
}

static const struct linkr_debugger_task_blob_entry *task_layout_find(
	const struct linkr_debugger_task_blob_layout *layout, const char *id)
{
	for (size_t i = 0U; i < layout->task_count; i++) {
		if (strcmp(layout->tasks[i].id, id) == 0) {
			return &layout->tasks[i];
		}
	}
	return NULL;
}

static bool task_manifest_basic_valid(const struct task_manifest *manifest)
{
	size_t present = 0U;
	bool order_seen[LINKR_DEBUGGER_TASK_MAX_TASKS] = { false };

	if (manifest->magic != TASK_MANIFEST_MAGIC ||
	    manifest->version != TASK_MANIFEST_VERSION ||
	    manifest->size != sizeof(*manifest) ||
	    manifest->full_ir_len > LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE ||
	    manifest->task_count > LINKR_DEBUGGER_TASK_MAX_TASKS) {
		return false;
	}
	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
		const struct task_manifest_slot *slot = &manifest->slots[i];

		if (slot->present == 0U) {
			continue;
		}
		present++;
		if (memchr(slot->id, '\0', sizeof(slot->id)) == NULL ||
		    !linkr_debugger_task_id_valid(slot->id, strlen(slot->id)) ||
		    slot->request_count > LINKR_DEBUGGER_TASK_MAX_REQUESTS ||
		    slot->ir_len > LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE ||
		    slot->ir_bank > 1U || slot->source_bank > 1U ||
		    slot->order >= manifest->task_count ||
		    order_seen[slot->order] ||
		    (slot->source_present != 0U &&
		     (slot->source_len == 0U ||
		      slot->source_len > LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE))) {
			return false;
		}
		order_seen[slot->order] = true;
		for (size_t j = i + 1U; j < LINKR_DEBUGGER_TASK_MAX_TASKS; j++) {
			if (manifest->slots[j].present != 0U &&
			    memchr(manifest->slots[j].id, '\0',
				   sizeof(manifest->slots[j].id)) != NULL &&
			    strcmp(slot->id, manifest->slots[j].id) == 0) {
				return false;
			}
		}
	}
	for (size_t i = 0U; i < manifest->task_count; i++) {
		if (!order_seen[i]) {
			return false;
		}
	}
	return present == manifest->task_count;
}

static bool task_source_bytes_valid(const uint8_t *source, size_t len)
{
	if (source == NULL || len == 0U || len > LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE ||
	    !linkr_debugger_json_utf8_valid((const char *)source, len)) {
		return false;
	}
	for (size_t i = 0U; i < len; i++) {
		uint8_t ch = source[i];

		if ((ch < 0x20U && ch != '\t' && ch != '\n' && ch != '\r') ||
		    ch == 0x7fU) {
			return false;
		}
	}
	return true;
}

static enum linkr_debugger_task_result task_save_error(int ret)
{
	return ret == -ENOSPC ? LINKR_DEBUGGER_TASK_STORAGE_FULL :
		LINKR_DEBUGGER_TASK_STORAGE_ERROR;
}

static enum linkr_debugger_task_result task_verify_saved(
	const char *name, size_t len, const uint8_t expected_digest[32])
{
	uint8_t digest[LINKR_DEBUGGER_TASK_SHA256_SIZE];
	int ret = task_backend_load_one(name, task_stage, sizeof(task_stage));

	if (ret < 0 || (size_t)ret != len) {
		return LINKR_DEBUGGER_TASK_READBACK_FAILED;
	}
	linkr_debugger_task_sha256_digest(task_stage, len, digest);
	return memcmp(digest, expected_digest, sizeof(digest)) == 0 ?
		LINKR_DEBUGGER_TASK_OK : LINKR_DEBUGGER_TASK_READBACK_FAILED;
}

static bool task_storage_measure_locked(struct linkr_debugger_task_storage *storage)
{
	uint32_t total = TASK_SETTINGS_SIZE;
	ssize_t free_bytes;

	memset(storage, 0, sizeof(*storage));
	storage->reserve_bytes = LINKR_DEBUGGER_TASK_STORAGE_RESERVE_BYTES;
	storage->task_bytes = sizeof(task_runtime.manifest);
	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
		const struct task_manifest_slot *slot =
			&task_runtime.manifest.slots[i];

		if (slot->present != 0U) {
			storage->task_bytes += slot->source_len + slot->ir_len;
		}
	}

#ifdef LINKR_DEBUGGER_TASK_HOST_TEST
	if (task_test_storage_free >= 0) {
		free_bytes = task_test_storage_free;
	} else {
		free_bytes = (ssize_t)total - (ssize_t)storage->task_bytes;
	}
#else
	void *storage_ptr = NULL;
	struct nvs_fs *fs;

	if (settings_storage_get(&storage_ptr) < 0 || storage_ptr == NULL) {
		storage->total_bytes = total;
		storage->free_bytes = total;
		storage->save_allowed = true;
		return false;
	}
	fs = storage_ptr;
	if (!fs->ready) {
		storage->total_bytes = total;
		storage->free_bytes = total;
		storage->save_allowed = true;
		return false;
	}
	total = (uint32_t)fs->sector_count * fs->sector_size;
	k_mutex_lock(&fs->nvs_lock, K_FOREVER);
	free_bytes = nvs_calc_free_space(fs);
	k_mutex_unlock(&fs->nvs_lock);
#endif

	if (free_bytes < 0) {
		storage->total_bytes = total;
		storage->free_bytes = total;
		storage->save_allowed = true;
		return false;
	}
	if ((uint32_t)free_bytes > total) {
		free_bytes = (ssize_t)total;
	}
	storage->total_bytes = total;
	storage->free_bytes = (uint32_t)free_bytes;
	storage->used_bytes = total - storage->free_bytes;
	if (storage->task_bytes > total) {
		storage->task_bytes = total;
	}
	storage->measured = true;
	storage->save_allowed = storage->free_bytes >= storage->reserve_bytes;
	return true;
}

static void task_status_build(void)
{
	struct linkr_debugger_task_status *status = &task_runtime.status;

	memset(status, 0, sizeof(*status));
	status->backend_available = task_runtime.backend_available;
	if (!task_runtime.backend_available) {
		status->storage.reserve_bytes =
			LINKR_DEBUGGER_TASK_STORAGE_RESERVE_BYTES;
		return;
	}
	memcpy(status->task_store_id, task_runtime.manifest.store_id,
	       sizeof(status->task_store_id));
	status->revision = task_runtime.manifest.revision;
	task_catalog_fingerprint(status->catalog_fingerprint);
	status->full_ir_len = task_runtime.manifest.full_ir_len;
	memcpy(status->full_ir_digest, task_runtime.manifest.full_ir_digest,
	       sizeof(status->full_ir_digest));
	status->task_count = task_runtime.manifest.task_count;
	for (uint8_t order = 0U; order < task_runtime.manifest.task_count; order++) {
		int slot_index = task_find_slot_by_order(&task_runtime.manifest, order);
		const struct task_manifest_slot *slot;
		struct linkr_debugger_task_summary *summary = &status->tasks[order];

		if (slot_index < 0) {
			continue;
		}
		slot = &task_runtime.manifest.slots[slot_index];

		summary->slot = (uint8_t)slot_index;
		memcpy(summary->id, slot->id, strlen(slot->id) + 1U);
		summary->request_count = slot->request_count;
		summary->source_present = slot->source_present != 0U;
		summary->source_len = slot->source_present != 0U ? slot->source_len : 0U;
		if (slot->source_present != 0U) {
			memcpy(summary->source_digest, slot->source_digest,
			       sizeof(summary->source_digest));
		}
		summary->ir_len = slot->ir_len;
		memcpy(summary->ir_digest, slot->ir_digest, sizeof(summary->ir_digest));
	}
	(void)task_storage_measure_locked(&status->storage);
}

/* Load one slot's standalone IR, validate it against the manifest and append
 * its task section to the assembled full IR. */
static bool task_load_slot_ir(uint8_t slot_index,
			      const struct task_manifest_slot *slot,
			      char *assembly, size_t *assembly_len)
{
	char name[TASK_KEY_NAME_SIZE];
	struct linkr_debugger_task_blob_layout layout;
	const struct linkr_debugger_task_blob_entry *entry;
	uint8_t digest[LINKR_DEBUGGER_TASK_SHA256_SIZE];
	size_t section_len;
	size_t ir_len;
	int ret;

	task_key_ir(name, slot_index, slot->ir_bank);
	ret = task_backend_load_one(name, task_stage, sizeof(task_stage));
	if (ret <= 0 || (size_t)ret > sizeof(task_stage)) {
		return false;
	}
	if (!linkr_debugger_task_blob_inspect(task_stage, (size_t)ret, false, &layout) ||
	    layout.task_count != 1U || strcmp(layout.tasks[0].id, slot->id) != 0 ||
	    layout.tasks[0].request_count != slot->request_count) {
		return false;
	}
	entry = &layout.tasks[0];
	ir_len = LINKR_DEBUGGER_TASK_BLOB_HEADER_LEN +
		(entry->digest_end - entry->section_start);
	task_digest_section(task_stage, entry, digest);
	if (ir_len != slot->ir_len ||
	    memcmp(digest, slot->ir_digest, sizeof(digest)) != 0) {
		return false;
	}
	section_len = entry->section_end - entry->section_start;
	if (*assembly_len + section_len > LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE) {
		return false;
	}
	memcpy(assembly + *assembly_len, task_stage + entry->section_start,
	       section_len);
	*assembly_len += section_len;
	return true;
}

static bool task_load_slot_source(uint8_t slot_index,
				  const struct task_manifest_slot *slot)
{
	char name[TASK_KEY_NAME_SIZE];
	uint8_t digest[LINKR_DEBUGGER_TASK_SHA256_SIZE];
	int ret;

	task_key_source(name, slot_index, slot->source_bank);
	ret = task_backend_load_one(name, task_stage, sizeof(task_stage));
	if (ret <= 0 || (size_t)ret != slot->source_len ||
	    (size_t)ret > sizeof(task_stage) ||
	    !task_source_bytes_valid((uint8_t *)task_stage, slot->source_len)) {
		return false;
	}
	linkr_debugger_task_sha256_digest(task_stage, slot->source_len, digest);
	return memcmp(digest, slot->source_digest, sizeof(digest)) == 0;
}

static bool task_load_and_validate(void)
{
	size_t assembly_len;
	uint8_t digest[LINKR_DEBUGGER_TASK_SHA256_SIZE];

	if (task_runtime.manifest.task_count == 0U) {
		if (task_runtime.manifest.full_ir_len != 0U) {
			return false;
		}
		linkr_debugger_task_sha256_digest(NULL, 0U, digest);
		if (memcmp(digest, task_runtime.manifest.full_ir_digest,
			   sizeof(digest)) != 0) {
			return false;
		}
		memset(&task_runtime.layout, 0, sizeof(task_runtime.layout));
		task_runtime.active_ir[0] = '\0';
		return true;
	}
	assembly_len = LINKR_DEBUGGER_TASK_BLOB_HEADER_LEN;
	memcpy(task_runtime.active_ir, LINKR_DEBUGGER_TASK_BLOB_HEADER, assembly_len);
	for (uint8_t order = 0U; order < task_runtime.manifest.task_count; order++) {
		int slot_index = task_find_slot_by_order(&task_runtime.manifest, order);
		const struct task_manifest_slot *slot;

		if (slot_index < 0) {
			return false;
		}
		slot = &task_runtime.manifest.slots[slot_index];
		if (!task_load_slot_ir((uint8_t)slot_index, slot,
				       task_runtime.active_ir, &assembly_len) ||
		    (slot->source_present != 0U &&
		     !task_load_slot_source((uint8_t)slot_index, slot))) {
			return false;
		}
	}
	if (assembly_len != task_runtime.manifest.full_ir_len) {
		return false;
	}
	linkr_debugger_task_sha256_digest(task_runtime.active_ir, assembly_len, digest);
	if (memcmp(digest, task_runtime.manifest.full_ir_digest, sizeof(digest)) != 0) {
		return false;
	}
	task_runtime.active_ir[assembly_len] = '\0';
	if (!linkr_debugger_task_blob_inspect(task_runtime.active_ir, assembly_len,
					      false, &task_runtime.layout) ||
	    task_runtime.layout.task_count != task_runtime.manifest.task_count) {
		return false;
	}
	for (uint8_t order = 0U; order < task_runtime.manifest.task_count; order++) {
		int slot_index = task_find_slot_by_order(&task_runtime.manifest, order);
		const struct task_manifest_slot *slot;
		const struct linkr_debugger_task_blob_entry *entry;

		if (slot_index < 0) {
			return false;
		}
		slot = &task_runtime.manifest.slots[slot_index];
		entry = task_layout_find(&task_runtime.layout, slot->id);
		if (entry == NULL || entry != &task_runtime.layout.tasks[order]) {
			return false;
		}
		task_digest_section(task_runtime.active_ir, entry, digest);
		if (entry->request_count != slot->request_count ||
		    LINKR_DEBUGGER_TASK_BLOB_HEADER_LEN +
				    (entry->digest_end - entry->section_start) !=
			    slot->ir_len ||
		    memcmp(digest, slot->ir_digest, sizeof(digest)) != 0) {
			return false;
		}
	}
	return true;
}

static bool task_initialize_empty_store(const uint8_t catalog[32])
{
	struct task_manifest manifest;

	memset(&manifest, 0, sizeof(manifest));
	manifest.magic = TASK_MANIFEST_MAGIC;
	manifest.version = TASK_MANIFEST_VERSION;
	manifest.size = sizeof(manifest);
	task_backend_random(manifest.store_id, sizeof(manifest.store_id));
	/* RFC 4122 variant and random-version UUID formatting. */
	manifest.store_id[6] = (uint8_t)((manifest.store_id[6] & 0x0fU) | 0x40U);
	manifest.store_id[8] = (uint8_t)((manifest.store_id[8] & 0x3fU) | 0x80U);
	memcpy(manifest.catalog_fingerprint, catalog,
	       sizeof(manifest.catalog_fingerprint));
	linkr_debugger_task_sha256_digest(NULL, 0U, manifest.full_ir_digest);
	if (task_backend_save_one(TASK_KEY_MANIFEST, &manifest, sizeof(manifest)) < 0) {
		return false;
	}
	task_runtime.manifest = manifest;
	memset(&task_runtime.layout, 0, sizeof(task_runtime.layout));
	task_runtime.active_ir[0] = '\0';
	return true;
}

void linkr_debugger_task_init(void)
{
	uint8_t catalog[LINKR_DEBUGGER_TASK_SHA256_SIZE];
	int ret;

	TASK_LOCK();
	task_status_reset();
	task_catalog_fingerprint(catalog);
	if (task_backend_subsys_init() < 0) {
		TASK_UNLOCK();
		return;
	}
	ret = task_backend_load_one(TASK_KEY_MANIFEST, &task_work.manifest,
				    sizeof(task_work.manifest));
	if (ret == 0) {
		if (!task_initialize_empty_store(catalog)) {
			TASK_UNLOCK();
			return;
		}
	} else if (ret != (int)sizeof(task_work.manifest) ||
		   !task_manifest_basic_valid(&task_work.manifest)) {
		TASK_UNLOCK();
		return;
	} else {
		task_runtime.manifest = task_work.manifest;
		if (!task_load_and_validate()) {
			task_status_reset();
			TASK_UNLOCK();
			return;
		}
	}
	task_runtime.backend_available = true;
	task_status_build();
	TASK_UNLOCK();
}

enum linkr_debugger_task_result linkr_debugger_task_status_get(
	struct linkr_debugger_task_status *status)
{
	if (status == NULL) {
		return LINKR_DEBUGGER_TASK_INVALID_ARGUMENT;
	}
	TASK_LOCK();
	task_status_build();
	*status = task_runtime.status;
	TASK_UNLOCK();
	return LINKR_DEBUGGER_TASK_OK;
}

enum linkr_debugger_task_result linkr_debugger_task_ir_snapshot(
	uint8_t *ir, size_t ir_cap, size_t *ir_len)
{
	if (ir == NULL || ir_len == NULL) {
		return LINKR_DEBUGGER_TASK_INVALID_ARGUMENT;
	}
	TASK_LOCK();
	if (!task_runtime.backend_available) {
		TASK_UNLOCK();
		return LINKR_DEBUGGER_TASK_BACKEND_UNAVAILABLE;
	}
	if (ir_cap < task_runtime.manifest.full_ir_len) {
		TASK_UNLOCK();
		return LINKR_DEBUGGER_TASK_INVALID_ARGUMENT;
	}
	memcpy(ir, task_runtime.active_ir, task_runtime.manifest.full_ir_len);
	*ir_len = task_runtime.manifest.full_ir_len;
	TASK_UNLOCK();
	return LINKR_DEBUGGER_TASK_OK;
}

enum linkr_debugger_task_result linkr_debugger_task_source_snapshot(
	uint8_t slot_index, uint8_t *source, size_t source_cap, size_t *source_len)
{
	const struct task_manifest_slot *slot;
	char name[TASK_KEY_NAME_SIZE];
	uint8_t digest[LINKR_DEBUGGER_TASK_SHA256_SIZE];
	int ret;

	if (source == NULL || source_len == NULL ||
	    slot_index >= LINKR_DEBUGGER_TASK_MAX_TASKS) {
		return LINKR_DEBUGGER_TASK_INVALID_ARGUMENT;
	}
	TASK_LOCK();
	if (!task_runtime.backend_available) {
		TASK_UNLOCK();
		return LINKR_DEBUGGER_TASK_BACKEND_UNAVAILABLE;
	}
	slot = &task_runtime.manifest.slots[slot_index];
	if (slot->present == 0U || slot->source_present == 0U) {
		TASK_UNLOCK();
		return LINKR_DEBUGGER_TASK_SOURCE_ABSENT;
	}
	if (source_cap < slot->source_len) {
		TASK_UNLOCK();
		return LINKR_DEBUGGER_TASK_INVALID_ARGUMENT;
	}
	task_key_source(name, slot_index, slot->source_bank);
	ret = task_backend_load_one(name, source, source_cap);
	if (ret < 0 || ret != (int)slot->source_len) {
		TASK_UNLOCK();
		return LINKR_DEBUGGER_TASK_READBACK_FAILED;
	}
	linkr_debugger_task_sha256_digest(source, slot->source_len, digest);
	if (memcmp(digest, slot->source_digest, sizeof(digest)) != 0) {
		TASK_UNLOCK();
		return LINKR_DEBUGGER_TASK_READBACK_FAILED;
	}
	*source_len = slot->source_len;
	TASK_UNLOCK();
	return LINKR_DEBUGGER_TASK_OK;
}

static bool task_collect_old_pieces(struct task_piece *pieces, size_t *count)
{
	*count = task_runtime.manifest.task_count;
	for (uint8_t order = 0U; order < task_runtime.manifest.task_count; order++) {
		int slot_index = task_find_slot_by_order(&task_runtime.manifest, order);
		const struct linkr_debugger_task_blob_entry *entry = task_layout_find(
			&task_runtime.layout,
			task_runtime.manifest.slots[slot_index].id);

		if (entry == NULL) {
			return false;
		}
		pieces[order].section = task_runtime.active_ir + entry->section_start;
		pieces[order].section_len = entry->section_end - entry->section_start;
	}
	return true;
}

static bool task_copy_pieces(const struct task_piece *pieces, size_t count,
			     char *out, size_t *out_len)
{
	size_t len = LINKR_DEBUGGER_TASK_BLOB_HEADER_LEN;

	if (count == 0U) {
		*out_len = 0U;
		return true;
	}
	memcpy(out, LINKR_DEBUGGER_TASK_BLOB_HEADER, len);
	for (size_t i = 0U; i < count; i++) {
		if (len + pieces[i].section_len > LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE) {
			return false;
		}
		memcpy(out + len, pieces[i].section, pieces[i].section_len);
		len += pieces[i].section_len;
	}
	*out_len = len;
	return true;
}

static void task_manifest_slot_clear(struct task_manifest_slot *slot)
{
	memset(slot, 0, sizeof(*slot));
}

static void task_slot_set_ir_metadata(struct task_manifest_slot *slot,
				      const char *ir,
				      const struct linkr_debugger_task_blob_entry *entry)
{
	slot->request_count = (uint16_t)entry->request_count;
	slot->ir_len = (uint16_t)(LINKR_DEBUGGER_TASK_BLOB_HEADER_LEN +
				 entry->digest_end - entry->section_start);
	task_digest_section(ir, entry, slot->ir_digest);
}

static bool task_prepare_import_slots(struct task_manifest *candidate,
				      const struct linkr_debugger_task_blob_layout *layout)
{
	struct task_manifest old = *candidate;

	memset(candidate->slots, 0, sizeof(candidate->slots));
	candidate->task_count = (uint8_t)layout->task_count;
	/* Preserve slots first so new IDs cannot steal a surviving task's slot. */
	for (size_t i = 0U; i < layout->task_count; i++) {
		int old_slot = task_find_manifest_slot(&old, layout->tasks[i].id);

		if (old_slot >= 0) {
			candidate->slots[old_slot] = old.slots[old_slot];
		}
	}
	for (size_t i = 0U; i < layout->task_count; i++) {
		if (task_find_manifest_slot(candidate, layout->tasks[i].id) < 0) {
			int free_slot = task_find_free_slot(candidate);

			if (free_slot < 0) {
				return false;
			}
			candidate->slots[free_slot].present = 1U;
			memcpy(candidate->slots[free_slot].id, layout->tasks[i].id,
			       strlen(layout->tasks[i].id) + 1U);
		}
	}
	return true;
}

static enum linkr_debugger_task_result task_prepare_candidate(
	const struct linkr_debugger_task_store_mutation *mutation)
{
	struct task_manifest *candidate = &task_work.manifest;
	struct task_piece *pieces = task_work.pieces;
	size_t piece_count = 0U;

	*candidate = task_runtime.manifest;
	memset(task_work.writes, 0, sizeof(task_work.writes));
	memset(task_work.delete_source_keys, 0, sizeof(task_work.delete_source_keys));
	memset(task_work.delete_slot_keys, 0, sizeof(task_work.delete_slot_keys));
	memset(&task_work.layout, 0, sizeof(task_work.layout));

	switch (mutation->opcode) {
	case LINKR_DEBUGGER_TASK_MUTATION_UPSERT: {
		/* Reuse the work-area layout as incoming scratch: it is
		 * overwritten with the committed output later in this function
		 * and keeps the shell clear call chain off a ~1 KiB frame.
		 */
		struct linkr_debugger_task_blob_layout *incoming = &task_work.layout;
		const struct linkr_debugger_task_blob_entry *entry;
		struct task_manifest_slot *slot;
		struct task_piece new_piece;
		int slot_index;
		uint8_t order;
		bool had_source;

		if (!linkr_debugger_task_id_valid(mutation->id, strlen(mutation->id)) ||
		    !linkr_debugger_task_blob_inspect(mutation->ir, mutation->ir_len,
						      false, incoming) ||
		    incoming->task_count != 1U ||
		    incoming->tasks[0].section_start !=
			    LINKR_DEBUGGER_TASK_BLOB_HEADER_LEN ||
		    strcmp(incoming->tasks[0].id, mutation->id) != 0 ||
		    (mutation->source_len > 0U &&
		     !task_source_bytes_valid(mutation->source, mutation->source_len)) ||
		    !task_collect_old_pieces(pieces, &piece_count)) {
			return LINKR_DEBUGGER_TASK_INVALID_TASK;
		}
		entry = &incoming->tasks[0];
		new_piece.section = mutation->ir + entry->section_start;
		new_piece.section_len = entry->section_end - entry->section_start;
		slot_index = task_find_manifest_slot(candidate, mutation->id);
		if (slot_index < 0) {
			slot_index = task_find_free_slot(candidate);
			if (slot_index < 0) {
				return LINKR_DEBUGGER_TASK_STORAGE_FULL;
			}
			task_manifest_slot_clear(&candidate->slots[slot_index]);
			candidate->slots[slot_index].present = 1U;
			memcpy(candidate->slots[slot_index].id, mutation->id,
			       strlen(mutation->id) + 1U);
			candidate->slots[slot_index].order = (uint8_t)piece_count;
			pieces[piece_count] = new_piece;
			order = (uint8_t)piece_count;
			piece_count++;
			candidate->task_count++;
		} else {
			order = candidate->slots[slot_index].order;
			pieces[order] = new_piece;
		}
		slot = &candidate->slots[slot_index];
		had_source = slot->source_present != 0U;
		task_slot_set_ir_metadata(slot, mutation->ir, entry);
		slot->source_present = mutation->source_len > 0U ? 1U : 0U;
		slot->source_len = (uint16_t)mutation->source_len;
		if (mutation->source_len > 0U) {
			linkr_debugger_task_sha256_digest(mutation->source,
				mutation->source_len, slot->source_digest);
			task_work.writes[slot_index].source_pending = true;
		} else {
			memset(slot->source_digest, 0, sizeof(slot->source_digest));
			task_work.delete_source_keys[slot_index] = had_source;
		}
		task_work.writes[slot_index].ir_pending = true;
		task_work.writes[slot_index].piece = new_piece;
		break;
	}
	case LINKR_DEBUGGER_TASK_MUTATION_DELETE: {
		int slot_index = task_find_manifest_slot(candidate, mutation->id);
		uint8_t removed_order;

		if (slot_index < 0 ||
		    !task_collect_old_pieces(pieces, &piece_count)) {
			return LINKR_DEBUGGER_TASK_INVALID_TASK;
		}
		removed_order = candidate->slots[slot_index].order;
		for (size_t i = removed_order; i + 1U < piece_count; i++) {
			pieces[i] = pieces[i + 1U];
		}
		piece_count--;
		for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
			struct task_manifest_slot *slot = &candidate->slots[i];

			if (slot->present != 0U && slot->order > removed_order) {
				slot->order--;
			}
		}
		task_manifest_slot_clear(&candidate->slots[slot_index]);
		candidate->task_count--;
		task_work.delete_slot_keys[slot_index] = true;
		break;
	}
	case LINKR_DEBUGGER_TASK_MUTATION_CLEAR:
		for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
			task_manifest_slot_clear(&candidate->slots[i]);
			task_work.delete_slot_keys[i] =
				task_runtime.manifest.slots[i].present != 0U;
		}
		candidate->task_count = 0U;
		break;
	case LINKR_DEBUGGER_TASK_MUTATION_IMPORT: {
		struct linkr_debugger_task_blob_layout *incoming = &task_work.layout;

		if (!linkr_debugger_task_blob_inspect(mutation->ir, mutation->ir_len,
						      false, incoming) ||
		    /* Canonical framing keeps raw import/export byte-exact. */
		    incoming->tasks[0].section_start !=
			    LINKR_DEBUGGER_TASK_BLOB_HEADER_LEN ||
		    !task_prepare_import_slots(candidate, incoming)) {
			return LINKR_DEBUGGER_TASK_INVALID_TASK;
		}
		for (size_t i = 0U; i < incoming->task_count; i++) {
			pieces[i].section = mutation->ir +
				incoming->tasks[i].section_start;
			pieces[i].section_len = incoming->tasks[i].section_end -
				incoming->tasks[i].section_start;
		}
		piece_count = incoming->task_count;
		for (size_t i = 0U; i < incoming->task_count; i++) {
			const struct linkr_debugger_task_blob_entry *entry =
				&incoming->tasks[i];
			int slot_index = task_find_manifest_slot(candidate, entry->id);
			struct task_manifest_slot *slot;
			bool had_source;
			int old_slot;

			if (slot_index < 0) {
				return LINKR_DEBUGGER_TASK_INVALID_TASK;
			}
			slot = &candidate->slots[slot_index];
			had_source = slot->source_present != 0U;
			slot->order = (uint8_t)i;
			task_slot_set_ir_metadata(slot, mutation->ir, entry);
			task_work.writes[slot_index].ir_pending = true;
			task_work.writes[slot_index].piece = pieces[i];
			/* Full import preserves a source only when the task's
			 * digest-relevant IR is unchanged. */
			if (had_source) {
				old_slot = task_find_manifest_slot(&task_runtime.manifest,
								   slot->id);
				if (old_slot < 0 ||
				    memcmp(slot->ir_digest,
					   task_runtime.manifest.slots[old_slot].ir_digest,
					   sizeof(slot->ir_digest)) != 0) {
					slot->source_present = 0U;
					slot->source_len = 0U;
					memset(slot->source_digest, 0,
					       sizeof(slot->source_digest));
					task_work.delete_source_keys[slot_index] = true;
				}
			}
		}
		for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
			if (task_runtime.manifest.slots[i].present != 0U &&
			    candidate->slots[i].present == 0U) {
				task_work.delete_slot_keys[i] = true;
			}
		}
		break;
	}
	default:
		return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
	}

	if (!task_copy_pieces(pieces, piece_count, task_work.full_ir,
			      &task_work.full_ir_len)) {
		return LINKR_DEBUGGER_TASK_INVALID_TASK;
	}
	if (task_work.full_ir_len == 0U) {
		memset(&task_work.layout, 0, sizeof(task_work.layout));
	} else if (!linkr_debugger_task_blob_inspect(task_work.full_ir,
						    task_work.full_ir_len, false,
						    &task_work.layout) ||
		   task_work.layout.task_count != candidate->task_count) {
		return LINKR_DEBUGGER_TASK_INVALID_TASK;
	}
	for (uint8_t order = 0U; order < candidate->task_count; order++) {
		int slot_index = task_find_slot_by_order(candidate, order);
		const struct task_manifest_slot *slot;
		const struct linkr_debugger_task_blob_entry *entry;
		uint8_t digest[LINKR_DEBUGGER_TASK_SHA256_SIZE];

		if (slot_index < 0) {
			return LINKR_DEBUGGER_TASK_INVALID_TASK;
		}
		slot = &candidate->slots[slot_index];
		entry = task_layout_find(&task_work.layout, slot->id);
		if (entry == NULL) {
			return LINKR_DEBUGGER_TASK_INVALID_TASK;
		}
		task_digest_section(task_work.full_ir, entry, digest);
		if (memcmp(digest, slot->ir_digest, sizeof(digest)) != 0) {
			return LINKR_DEBUGGER_TASK_INVALID_TASK;
		}
	}
	/* Banks belong to physical slots, not IDs. A full replacement may have
	 * cleared all candidate metadata while the committed manifest still
	 * references those slots. Retries must also use that committed bank. */
	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
		const struct task_manifest_slot *old = &task_runtime.manifest.slots[i];
		struct task_manifest_slot *slot = &candidate->slots[i];

		if (task_work.writes[i].ir_pending) {
			slot->ir_bank = (uint8_t)(1U - old->ir_bank);
		}
		if (task_work.writes[i].source_pending) {
			slot->source_bank = (uint8_t)(1U - old->source_bank);
		}
		if (old->source_present && !slot->source_present) {
			task_work.delete_source_keys[i] = true;
		}
	}
	candidate->full_ir_len = (uint16_t)task_work.full_ir_len;
	linkr_debugger_task_sha256_digest(task_work.full_ir, task_work.full_ir_len,
					  candidate->full_ir_digest);
	return LINKR_DEBUGGER_TASK_OK;
}

static void task_delete_keys(bool delete_slot, bool delete_source,
			     uint8_t slot_index)
{
	char name[TASK_KEY_NAME_SIZE];

	if (delete_source || delete_slot) {
		for (uint8_t bank = 0U; bank < 2U; bank++) {
			task_key_source(name, slot_index, bank);
			(void)task_backend_delete_one(name);
		}
	}
	if (delete_slot) {
		for (uint8_t bank = 0U; bank < 2U; bank++) {
			task_key_ir(name, slot_index, bank);
			(void)task_backend_delete_one(name);
		}
	}
}


enum linkr_debugger_task_result linkr_debugger_task_store_clear(void)
{
	struct task_manifest *candidate = &task_work.manifest;
	enum linkr_debugger_task_result result;
	int ret;

	TASK_LOCK();
	if (!task_runtime.backend_available) {
		result = LINKR_DEBUGGER_TASK_BACKEND_UNAVAILABLE;
		goto done;
	}
	if (task_runtime.manifest.revision == UINT64_MAX) {
		result = LINKR_DEBUGGER_TASK_INVALID_MUTATION;
		goto done;
	}

	*candidate = task_runtime.manifest;
	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
		task_manifest_slot_clear(&candidate->slots[i]);
	}
	candidate->task_count = 0U;
	candidate->revision = task_runtime.manifest.revision + 1U;
	candidate->full_ir_len = 0U;
	linkr_debugger_task_sha256_digest(NULL, 0U, candidate->full_ir_digest);

	ret = task_backend_save_one(TASK_KEY_MANIFEST, candidate, sizeof(*candidate));
	if (ret < 0) {
		result = task_save_error(ret);
		goto done;
	}

	task_runtime.manifest = *candidate;
	memset(&task_runtime.layout, 0, sizeof(task_runtime.layout));
	task_runtime.active_ir[0] = '\0';
	task_status_build();
	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
		task_delete_keys(true, true, (uint8_t)i);
	}
	result = LINKR_DEBUGGER_TASK_OK;

done:
	TASK_UNLOCK();
	return result;
}

static size_t task_required_write_bytes(const struct task_manifest *candidate)
{
	size_t required = sizeof(*candidate) + TASK_STORAGE_KEY_OVERHEAD;

	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
		if (task_work.writes[i].source_pending) {
			required += candidate->slots[i].source_len +
				    TASK_STORAGE_KEY_OVERHEAD;
		}
		if (task_work.writes[i].ir_pending) {
			required += candidate->slots[i].ir_len +
				    TASK_STORAGE_KEY_OVERHEAD;
		}
	}
	return required;
}

static bool task_storage_allows_write(size_t required)
{
	struct linkr_debugger_task_storage storage;

	if (!task_storage_measure_locked(&storage)) {
		return true;
	}
	return storage.free_bytes >=
	       storage.reserve_bytes + (uint32_t)required;
}

enum linkr_debugger_task_result linkr_debugger_task_store_apply(
	const struct linkr_debugger_task_store_mutation *mutation,
	struct linkr_debugger_task_mutation_outcome *outcome)
{
	struct task_manifest *candidate = &task_work.manifest;
	uint8_t current_catalog[LINKR_DEBUGGER_TASK_SHA256_SIZE];
	uint8_t digest[LINKR_DEBUGGER_TASK_SHA256_SIZE];
	enum linkr_debugger_task_result result;
	int affected_slot = -1;
	char name[TASK_KEY_NAME_SIZE];
	size_t standalone_len;
	int ret;

	if (mutation == NULL || outcome == NULL) {
		return LINKR_DEBUGGER_TASK_INVALID_ARGUMENT;
	}
	TASK_LOCK();
	memset(outcome, 0, sizeof(*outcome));
	outcome->status = task_runtime.status;
	if (!task_runtime.backend_available) {
		result = LINKR_DEBUGGER_TASK_BACKEND_UNAVAILABLE;
		goto done;
	}
	task_catalog_fingerprint(current_catalog);
	if (mutation->expected_revision != task_runtime.manifest.revision) {
		result = LINKR_DEBUGGER_TASK_REVISION_CONFLICT;
		goto done;
	}
	if (memcmp(mutation->expected_catalog, current_catalog,
		   sizeof(current_catalog)) != 0) {
		result = LINKR_DEBUGGER_TASK_CATALOG_CONFLICT;
		goto done;
	}
	if (task_runtime.manifest.revision == UINT64_MAX) {
		result = LINKR_DEBUGGER_TASK_INVALID_MUTATION;
		goto done;
	}
	result = task_prepare_candidate(mutation);
	if (result != LINKR_DEBUGGER_TASK_OK) {
		goto done;
	}
	candidate->revision = task_runtime.manifest.revision + 1U;
	memcpy(candidate->catalog_fingerprint, current_catalog,
	       sizeof(candidate->catalog_fingerprint));

	if ((mutation->opcode == LINKR_DEBUGGER_TASK_MUTATION_UPSERT ||
	     mutation->opcode == LINKR_DEBUGGER_TASK_MUTATION_IMPORT) &&
	    !task_storage_allows_write(task_required_write_bytes(candidate))) {
		result = LINKR_DEBUGGER_TASK_STORAGE_FULL;
		goto done;
	}

	/* Source keys first, then IR keys; the manifest commit is strictly last. */
	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
		if (!task_work.writes[i].source_pending) {
			continue;
		}
		task_key_source(name, (uint8_t)i, candidate->slots[i].source_bank);
		linkr_debugger_task_sha256_digest(mutation->source,
						  mutation->source_len, digest);
		ret = task_backend_save_one(name, mutation->source,
					    mutation->source_len);
		if (ret < 0) {
			result = task_save_error(ret);
			goto done;
		}
		result = task_verify_saved(name, mutation->source_len, digest);
		if (result != LINKR_DEBUGGER_TASK_OK) {
			goto done;
		}
	}
	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
		if (!task_work.writes[i].ir_pending) {
			continue;
		}
		standalone_len = LINKR_DEBUGGER_TASK_BLOB_HEADER_LEN +
			task_work.writes[i].piece.section_len;
		if (standalone_len > sizeof(task_stage)) {
			result = LINKR_DEBUGGER_TASK_INVALID_TASK;
			goto done;
		}
		memcpy(task_stage, LINKR_DEBUGGER_TASK_BLOB_HEADER,
		       LINKR_DEBUGGER_TASK_BLOB_HEADER_LEN);
		memcpy(task_stage + LINKR_DEBUGGER_TASK_BLOB_HEADER_LEN,
		       task_work.writes[i].piece.section,
		       task_work.writes[i].piece.section_len);
		linkr_debugger_task_sha256_digest(task_stage, standalone_len, digest);
		task_key_ir(name, (uint8_t)i, candidate->slots[i].ir_bank);
		ret = task_backend_save_one(name, task_stage, standalone_len);
		if (ret < 0) {
			result = task_save_error(ret);
			goto done;
		}
		result = task_verify_saved(name, standalone_len, digest);
		if (result != LINKR_DEBUGGER_TASK_OK) {
			goto done;
		}
	}
	/* The only visibility/linearization point: the manifest is always last. */
	ret = task_backend_save_one(TASK_KEY_MANIFEST, candidate, sizeof(*candidate));
	if (ret < 0) {
		result = task_save_error(ret);
		goto done;
	}
	task_runtime.manifest = *candidate;
	task_runtime.layout = task_work.layout;
	memcpy(task_runtime.active_ir, task_work.full_ir, task_work.full_ir_len);
	task_runtime.active_ir[task_work.full_ir_len] = '\0';
	task_status_build();
	outcome->status = task_runtime.status;
	if (mutation->opcode == LINKR_DEBUGGER_TASK_MUTATION_UPSERT) {
		affected_slot = task_find_manifest_slot(&task_runtime.manifest,
							mutation->id);
	}
	if (affected_slot >= 0) {
		outcome->affected_task_present = true;
		outcome->affected_slot = (uint8_t)affected_slot;
	}
	result = LINKR_DEBUGGER_TASK_OK;
	/* Post-commit cleanup only touches keys the new manifest ignores. */
	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
		task_delete_keys(task_work.delete_slot_keys[i],
				 task_work.delete_source_keys[i], (uint8_t)i);
	}

done:
	TASK_UNLOCK();
	return result;
}

#ifdef LINKR_DEBUGGER_TASK_HOST_TEST
void linkr_debugger_task_test_set_storage_free(ssize_t free_bytes)
{
	task_test_storage_free = free_bytes;
}

void linkr_debugger_task_test_force_revision(uint64_t revision)
{
	TASK_LOCK();
	task_runtime.manifest.revision = revision;
	task_status_build();
	TASK_UNLOCK();
}
#endif
