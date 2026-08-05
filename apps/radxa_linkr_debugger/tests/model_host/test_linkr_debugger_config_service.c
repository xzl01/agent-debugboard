#define _POSIX_C_SOURCE 200809L

#include "../../src/linkr_debugger_config_service_internal.h"
#include "../../src/linkr_debugger_control.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ARRAY_SIZE_LOCAL(array) (sizeof(array) / sizeof((array)[0]))
#define TEST_ERR_IO (-5)
#define TEST_ERR_REMOTE_IO (-121)
#define SETTER_NEVER ((size_t)-1)
#define TRACE_CAPACITY 80U
#define BLOCK_POLL_ROUNDS 200U

extern size_t linkr_debugger_config_service_capture_release_failures;
extern size_t linkr_debugger_config_service_flash_release_failures;

static void make_save_request(struct linkr_debugger_config_save_request *request,
			      bool reverse, bool confirmed);

static const uint8_t gpio_ids[] = {
	7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U,
	15U, 16U, 17U, 18U, 19U, 20U, 29U,
};

struct fake_env {
	struct linkr_debugger_control_item_state control_items[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
	int control_snapshot_result;
	size_t control_snapshot_calls;
	size_t control_row_count_override;
	bool control_snapshot_returned;
	unsigned control_lock_depth;
	unsigned control_lock_max_depth;
	struct linkr_debugger_config_entry setter_trace[TRACE_CAPACITY];
	size_t setter_calls;
	size_t setter_fail_at;
	int setter_failure_errno;

	enum linkr_debugger_config_store_result store_status_result;
	struct linkr_debugger_config_store_status store_status;
	enum linkr_debugger_config_store_result store_snapshot_result;
	struct linkr_debugger_config_snapshot store_snapshot;
	enum linkr_debugger_config_store_result store_save_result;
	enum linkr_debugger_config_store_result store_clear_result;
	size_t store_status_calls;
	size_t store_snapshot_calls;
	size_t store_save_calls;
	size_t store_clear_calls;
	unsigned store_save_depth;
	unsigned store_save_max_depth;
	struct linkr_debugger_config_snapshot store_last_saved;

	bool block_save;
	bool save_entered;
	bool save_release;

	bool capture_busy;
	bool flash_busy;
	bool capture_release_ok;
	bool flash_release_ok;
	unsigned capture_held;
	unsigned flash_held;
	size_t capture_acquire_calls;
	size_t capture_release_calls;
	size_t flash_acquire_calls;
	size_t flash_release_calls;
};

static pthread_mutex_t blocking_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t blocking_cond = PTHREAD_COND_INITIALIZER;

static size_t catalog_index(const struct linkr_debugger_config_item_desc *item)
{
	assert(item != NULL);
	return (size_t)(item - linkr_debugger_config_items);
}

static void append_entry(struct linkr_debugger_config_snapshot *snapshot,
			 uint8_t domain, uint8_t item_id, uint8_t value)
{
	struct linkr_debugger_config_entry *entry;

	assert(snapshot->entry_count < LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	entry = &snapshot->entries[snapshot->entry_count++];
	entry->domain = domain;
	entry->item_id = item_id;
	entry->value = value;
}

static void build_full_ordered(struct linkr_debugger_config_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	for (size_t i = 0U; i < ARRAY_SIZE_LOCAL(gpio_ids); i += 2U) {
		append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, gpio_ids[i], 0U);
	}
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID,
		     LINKR_DEBUGGER_CONFIG_SD_USB_READER);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID,
		     LINKR_DEBUGGER_CONFIG_TF_WP_PROTECTED);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID,
		     LINKR_DEBUGGER_CONFIG_USB_TARGET);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_VDD_5V_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_ON);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_ON);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_OFF);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_20V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_OFF);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID,
		     LINKR_DEBUGGER_CONFIG_VIN_1V8);
	for (size_t i = 1U; i < ARRAY_SIZE_LOCAL(gpio_ids); i += 2U) {
		append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, gpio_ids[i],
			     LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT |
			     (uint8_t)((gpio_ids[i] & 2U) != 0U ?
				LINKR_DEBUGGER_CONFIG_GPIO_LEVEL : 0U));
	}
	assert(snapshot->entry_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
}

static bool entries_equal(const struct linkr_debugger_config_entry *left,
			  const struct linkr_debugger_config_entry *right)
{
	return left->domain == right->domain && left->item_id == right->item_id &&
		left->value == right->value;
}

static void seed_default_control(struct fake_env *env)
{
	static const uint8_t switch_defaults[] = {
		LINKR_DEBUGGER_CONFIG_SD_TARGET,
		LINKR_DEBUGGER_CONFIG_USB_TARGET,
		LINKR_DEBUGGER_CONFIG_TF_WP_WRITABLE,
		LINKR_DEBUGGER_CONFIG_VIN_3V3,
	};

	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		const struct linkr_debugger_config_item_desc *item =
			&linkr_debugger_config_items[i];
		struct linkr_debugger_control_item_state *state = &env->control_items[i];

		state->domain = item->domain;
		state->item_id = item->item_id;
		state->value = 0U;
		state->available = true;
		if (item->domain == LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH) {
			assert(item->item_id >= 1U && item->item_id <= 4U);
			state->value = switch_defaults[item->item_id - 1U];
		}
	}
}

static void reset_env(struct fake_env *env)
{
	memset(env, 0, sizeof(*env));
	env->control_snapshot_result = 0;
	env->setter_fail_at = SETTER_NEVER;
	env->setter_failure_errno = TEST_ERR_IO;
	env->store_status_result = LINKR_DEBUGGER_CONFIG_STORE_OK;
	env->store_snapshot_result = LINKR_DEBUGGER_CONFIG_STORE_OK;
	env->store_save_result = LINKR_DEBUGGER_CONFIG_STORE_OK;
	env->store_clear_result = LINKR_DEBUGGER_CONFIG_STORE_OK;
	env->capture_release_ok = true;
	env->flash_release_ok = true;
	seed_default_control(env);
	linkr_debugger_config_service_capture_release_failures = 0U;
	linkr_debugger_config_service_flash_release_failures = 0U;
}

static void script_store_absent(struct fake_env *env)
{
	env->store_status.backend_available = true;
	env->store_status.presence = LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_ABSENT;
	env->store_status.snapshot_valid = false;
	env->store_status.reason = LINKR_DEBUGGER_CONFIG_STORE_REASON_ABSENT;
}

static void script_store_ready(struct fake_env *env,
			       const struct linkr_debugger_config_snapshot *snapshot)
{
	env->store_status.backend_available = true;
	env->store_status.presence = LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_PRESENT;
	env->store_status.snapshot_valid = true;
	env->store_status.reason = LINKR_DEBUGGER_CONFIG_STORE_REASON_READY;
	env->store_status.snapshot = *snapshot;
	env->store_snapshot = *snapshot;
}

static void script_store_reason(struct fake_env *env,
				enum linkr_debugger_config_store_reason reason)
{
	env->store_status.reason = reason;
	env->store_status.backend_available =
		reason != LINKR_DEBUGGER_CONFIG_STORE_REASON_BACKEND_UNAVAILABLE;
	env->store_status.presence =
		reason == LINKR_DEBUGGER_CONFIG_STORE_REASON_ABSENT ?
			LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_ABSENT :
			LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_UNKNOWN;
	env->store_status.snapshot_valid = false;
}

static int fake_control_snapshot_get(void *context,
				     struct linkr_debugger_control_snapshot *snapshot)
{
	struct fake_env *env = context;
	size_t count = env->control_row_count_override != 0U ?
			       env->control_row_count_override :
			       linkr_debugger_config_item_count;
	size_t fill = count < linkr_debugger_config_item_count ?
			      count : linkr_debugger_config_item_count;

	env->control_lock_depth++;
	if (env->control_lock_depth > env->control_lock_max_depth) {
		env->control_lock_max_depth = env->control_lock_depth;
	}
	env->control_snapshot_calls++;
	memset(snapshot, 0, sizeof(*snapshot));
	if (env->control_snapshot_result == 0) {
		snapshot->item_count = count;
		for (size_t i = 0U; i < fill; i++) {
			snapshot->items[i] = env->control_items[i];
		}
	}
	env->control_lock_depth--;
	env->control_snapshot_returned = true;
	return env->control_snapshot_result;
}

static int fake_setter(void *context, const struct linkr_debugger_config_entry *entry)
{
	struct fake_env *env = context;
	size_t call_index = env->setter_calls;

	assert(call_index < TRACE_CAPACITY);
	env->setter_trace[env->setter_calls++] = *entry;
	if (call_index == env->setter_fail_at) {
		return env->setter_failure_errno;
	}
	return 0;
}

static enum linkr_debugger_config_store_result fake_store_status_get(
	void *context, struct linkr_debugger_config_store_status *status)
{
	struct fake_env *env = context;

	env->store_status_calls++;
	*status = env->store_status;
	return env->store_status_result;
}

static enum linkr_debugger_config_store_result fake_store_snapshot_get(
	void *context, struct linkr_debugger_config_snapshot *snapshot)
{
	struct fake_env *env = context;

	env->store_snapshot_calls++;
	memset(snapshot, 0, sizeof(*snapshot));
	if (env->store_snapshot_result == LINKR_DEBUGGER_CONFIG_STORE_OK) {
		*snapshot = env->store_snapshot;
	}
	return env->store_snapshot_result;
}

static enum linkr_debugger_config_store_result fake_store_save(
	void *context, const struct linkr_debugger_config_snapshot *snapshot)
{
	struct fake_env *env = context;

	env->store_save_calls++;
	env->store_save_depth++;
	if (env->store_save_depth > env->store_save_max_depth) {
		env->store_save_max_depth = env->store_save_depth;
	}
	if (env->block_save) {
		assert(pthread_mutex_lock(&blocking_mutex) == 0);
		env->save_entered = true;
		assert(pthread_cond_broadcast(&blocking_cond) == 0);
		while (!env->save_release) {
			assert(pthread_cond_wait(&blocking_cond, &blocking_mutex) == 0);
		}
		assert(pthread_mutex_unlock(&blocking_mutex) == 0);
	}
	env->store_last_saved = *snapshot;
	env->store_save_depth--;
	return env->store_save_result;
}

static enum linkr_debugger_config_store_result fake_store_clear(void *context)
{
	struct fake_env *env = context;

	env->store_clear_calls++;
	return env->store_clear_result;
}

static bool fake_capture_try_acquire(void *context)
{
	struct fake_env *env = context;

	env->capture_acquire_calls++;
	if (env->capture_busy) {
		return false;
	}
	assert(env->capture_held == 0U);
	env->capture_held++;
	return true;
}

static bool fake_capture_release(void *context)
{
	struct fake_env *env = context;

	env->capture_release_calls++;
	assert(env->capture_held == 1U);
	env->capture_held--;
	return env->capture_release_ok;
}

static bool fake_flash_try_acquire(void *context)
{
	struct fake_env *env = context;

	env->flash_acquire_calls++;
	if (env->flash_busy) {
		return false;
	}
	assert(env->flash_held == 0U);
	env->flash_held++;
	return true;
}

static bool fake_flash_release(void *context)
{
	struct fake_env *env = context;

	env->flash_release_calls++;
	assert(env->flash_held == 1U);
	env->flash_held--;
	return env->flash_release_ok;
}

static void build_ops(struct fake_env *env,
		      struct linkr_debugger_config_service_ops *ops)
{
	(void)env;
	memset(ops, 0, sizeof(*ops));
	ops->control_snapshot_get = fake_control_snapshot_get;
	ops->control_replay_entry = fake_setter;
	ops->store_status_get = fake_store_status_get;
	ops->store_snapshot_get = fake_store_snapshot_get;
	ops->store_save = fake_store_save;
	ops->store_clear = fake_store_clear;
	ops->capture_try_acquire = fake_capture_try_acquire;
	ops->capture_release = fake_capture_release;
	ops->flash_try_acquire = fake_flash_try_acquire;
	ops->flash_release = fake_flash_release;
}

static struct fake_env *production_env;

static int production_snapshot_get(void *context,
				   struct linkr_debugger_control_snapshot *snapshot)
{
	(void)context;
	return fake_control_snapshot_get(production_env, snapshot);
}

static int production_replay_entry(void *context,
				  const struct linkr_debugger_config_entry *entry)
{
	(void)context;
	return fake_setter(production_env, entry);
}

static enum linkr_debugger_config_store_result production_status_get(
	void *context, struct linkr_debugger_config_store_status *status)
{
	(void)context;
	return fake_store_status_get(production_env, status);
}

static enum linkr_debugger_config_store_result production_snapshot_load(
	void *context, struct linkr_debugger_config_snapshot *snapshot)
{
	(void)context;
	return fake_store_snapshot_get(production_env, snapshot);
}

static enum linkr_debugger_config_store_result production_store_save(
	void *context, const struct linkr_debugger_config_snapshot *snapshot)
{
	(void)context;
	return fake_store_save(production_env, snapshot);
}

static enum linkr_debugger_config_store_result production_store_clear(void *context)
{
	(void)context;
	return fake_store_clear(production_env);
}

static bool production_capture_try(void *context)
{
	(void)context;
	return fake_capture_try_acquire(production_env);
}

static bool production_capture_done(void *context)
{
	(void)context;
	return fake_capture_release(production_env);
}

static bool production_flash_try(void *context)
{
	(void)context;
	return fake_flash_try_acquire(production_env);
}

static bool production_flash_done(void *context)
{
	(void)context;
	return fake_flash_release(production_env);
}

const struct linkr_debugger_config_service_ops
	linkr_debugger_config_service_production_ops = {
		.control_snapshot_get = production_snapshot_get,
		.control_replay_entry = production_replay_entry,
		.store_status_get = production_status_get,
		.store_snapshot_get = production_snapshot_load,
		.store_save = production_store_save,
		.store_clear = production_store_clear,
		.capture_try_acquire = production_capture_try,
		.capture_release = production_capture_done,
		.flash_try_acquire = production_flash_try,
		.flash_release = production_flash_done,
	};

static enum linkr_debugger_config_service_result init_fresh(struct fake_env *env)
{
	struct linkr_debugger_config_service_ops ops;

	build_ops(env, &ops);
	return linkr_debugger_config_service_init_with_ops(&ops, env);
}

static void assert_owners_idle(const struct fake_env *env)
{
	assert(env->capture_held == 0U);
	assert(env->flash_held == 0U);
	assert(linkr_debugger_config_service_capture_release_failures == 0U);
	assert(linkr_debugger_config_service_flash_release_failures == 0U);
}

static void assert_uninitialized_status(void)
{
	struct linkr_debugger_config_service_status status;

	memset(&status, 0xa5, sizeof(status));
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(!status.available);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNINITIALIZED);
	assert(!status.snapshot_present);
	assert(status.snapshot_version == 0U);
	assert(status.item_count == linkr_debugger_config_item_count);
	assert(status.saved_count == 0U);
	assert(status.applied_count == 0U);
	assert(status.pending_count == 0U);
	assert(status.failed_count == 0U);
	assert(status.failed_item == NULL);
	assert(status.failed_errno == 0);
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		assert(status.items[i].item == &linkr_debugger_config_items[i]);
		assert(!status.items[i].current_available);
		assert(!status.items[i].saved);
		assert(status.items[i].apply_state ==
		       LINKR_DEBUGGER_CONFIG_APPLY_NOT_SAVED);
	}
}

static void test_ops_validation_and_pre_init_contract(void)
{
	static const size_t ops_offsets[] = {
		offsetof(struct linkr_debugger_config_service_ops, control_snapshot_get),
		offsetof(struct linkr_debugger_config_service_ops, control_replay_entry),
		offsetof(struct linkr_debugger_config_service_ops, store_status_get),
		offsetof(struct linkr_debugger_config_service_ops, store_snapshot_get),
		offsetof(struct linkr_debugger_config_service_ops, store_save),
		offsetof(struct linkr_debugger_config_service_ops, store_clear),
		offsetof(struct linkr_debugger_config_service_ops, capture_try_acquire),
		offsetof(struct linkr_debugger_config_service_ops, capture_release),
		offsetof(struct linkr_debugger_config_service_ops, flash_try_acquire),
		offsetof(struct linkr_debugger_config_service_ops, flash_release),
	};
	struct fake_env env;
	struct linkr_debugger_config_service_ops ops;
	struct linkr_debugger_config_save_request request = {0};
	struct linkr_debugger_config_operation_report report;

	reset_env(&env);
	script_store_absent(&env);
	assert_uninitialized_status();
	assert(linkr_debugger_config_service_init_with_ops(NULL, &env) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert_uninitialized_status();
	for (size_t i = 0U; i < ARRAY_SIZE_LOCAL(ops_offsets); i++) {
		void *null_fn = NULL;

		build_ops(&env, &ops);
		memcpy((char *)&ops + ops_offsets[i], &null_fn, sizeof(null_fn));
		assert(linkr_debugger_config_service_init_with_ops(&ops, &env) ==
		       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	}
	assert_uninitialized_status();
	assert(env.store_status_calls == 0U);
	assert(env.control_snapshot_calls == 0U);

	request.item_count = 1U;
	request.item_ids[0] = linkr_debugger_config_items[0].id;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert(linkr_debugger_config_service_clear() ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert_uninitialized_status();
	assert_owners_idle(&env);
}

static void test_init_maps_every_store_state(void)
{
	static const struct {
		enum linkr_debugger_config_store_reason store_reason;
		enum linkr_debugger_config_service_result result;
		enum linkr_debugger_config_service_reason reason;
		bool available;
	} cases[] = {
		{ LINKR_DEBUGGER_CONFIG_STORE_REASON_ABSENT,
		  LINKR_DEBUGGER_CONFIG_SERVICE_OK,
		  LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT, true },
		{ LINKR_DEBUGGER_CONFIG_STORE_REASON_BACKEND_UNAVAILABLE,
		  LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE,
		  LINKR_DEBUGGER_CONFIG_SERVICE_REASON_BACKEND_UNAVAILABLE, false },
		{ LINKR_DEBUGGER_CONFIG_STORE_REASON_STORAGE_ERROR,
		  LINKR_DEBUGGER_CONFIG_SERVICE_STORAGE_ERROR,
		  LINKR_DEBUGGER_CONFIG_SERVICE_REASON_STORAGE_ERROR, false },
		{ LINKR_DEBUGGER_CONFIG_STORE_REASON_INVALID_SNAPSHOT,
		  LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT,
		  LINKR_DEBUGGER_CONFIG_SERVICE_REASON_INVALID_SNAPSHOT, false },
		{ LINKR_DEBUGGER_CONFIG_STORE_REASON_UNSUPPORTED_VERSION,
		  LINKR_DEBUGGER_CONFIG_SERVICE_UNSUPPORTED_VERSION,
		  LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNSUPPORTED_VERSION, false },
	};

	for (size_t i = 0U; i < ARRAY_SIZE_LOCAL(cases); i++) {
		struct fake_env env;
		struct linkr_debugger_config_service_status status;

		reset_env(&env);
		script_store_reason(&env, cases[i].store_reason);
		assert(init_fresh(&env) == cases[i].result);
		assert(env.store_status_calls == 1U);
		assert(env.store_snapshot_calls == 0U);
		assert(env.setter_calls == 0U);
		assert(env.capture_acquire_calls == 0U);
		assert(env.flash_acquire_calls == 0U);
		assert(linkr_debugger_config_service_status_get(&status) ==
		       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
		assert(status.available == cases[i].available);
		assert(status.reason == cases[i].reason);
		assert(!status.snapshot_present);
		assert(status.item_count == linkr_debugger_config_item_count);
		assert(status.saved_count == 0U);
		for (size_t j = 0U; j < linkr_debugger_config_item_count; j++) {
			assert(!status.items[j].saved);
			assert(status.items[j].apply_state ==
			       LINKR_DEBUGGER_CONFIG_APPLY_NOT_SAVED);
		}
		assert_owners_idle(&env);
	}

	{
		struct fake_env env;
		struct linkr_debugger_config_service_status status;

		reset_env(&env);
		script_store_absent(&env);
		env.store_status_result = LINKR_DEBUGGER_CONFIG_STORE_BACKEND_UNAVAILABLE;
		assert(init_fresh(&env) ==
		       LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE);
		assert(linkr_debugger_config_service_status_get(&status) ==
		       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
		assert(!status.available);
		assert(status.reason ==
		       LINKR_DEBUGGER_CONFIG_SERVICE_REASON_BACKEND_UNAVAILABLE);
	}
}

static void test_init_replays_full_snapshot_including_dangerous(void)
{
	struct fake_env env;
	struct linkr_debugger_config_snapshot full;
	struct linkr_debugger_config_service_status status;

	build_full_ordered(&full);
	reset_env(&env);
	script_store_ready(&env, &full);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(env.store_status_calls == 1U);
	assert(env.store_snapshot_calls == 1U);
	assert(env.store_save_calls == 0U);
	assert(env.setter_calls == full.entry_count);
	for (size_t i = 0U; i < full.entry_count; i++) {
		assert(entries_equal(&env.setter_trace[i], &full.entries[i]));
	}
	assert(env.capture_acquire_calls == 1U);
	assert(env.capture_release_calls == 1U);
	assert(env.flash_acquire_calls == 1U);
	assert(env.flash_release_calls == 1U);
	assert_owners_idle(&env);

	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.available);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY);
	assert(status.snapshot_present);
	assert(status.snapshot_version == LINKR_DEBUGGER_CONFIG_VERSION);
	assert(status.saved_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(status.applied_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(status.pending_count == 0U);
	assert(status.failed_count == 0U);
	assert(status.failed_item == NULL);
	for (size_t i = 0U; i < full.entry_count; i++) {
		bool requires_confirmation;
		size_t index = catalog_index(linkr_debugger_config_find_item(
			full.entries[i].domain, full.entries[i].item_id));

		assert(linkr_debugger_config_classify_entry(
			       &full.entries[i], &requires_confirmation) ==
		       LINKR_DEBUGGER_CONFIG_CODEC_OK);
		assert(status.items[index].saved);
		assert(status.items[index].saved_value == full.entries[i].value);
		assert(status.items[index].saved_requires_confirmation ==
		       requires_confirmation);
		assert(status.items[index].apply_state ==
		       LINKR_DEBUGGER_CONFIG_APPLY_APPLIED);
	}
}

static void test_init_replays_snapshot_on_every_boot(void)
{
	struct fake_env env;
	struct linkr_debugger_config_snapshot full;
	struct linkr_debugger_config_service_status status;

	build_full_ordered(&full);
	reset_env(&env);
	script_store_ready(&env, &full);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(env.setter_calls == full.entry_count);
	for (size_t i = 0U; i < full.entry_count; i++) {
		assert(entries_equal(&env.setter_trace[i], &full.entries[i]));
	}
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.snapshot_version == LINKR_DEBUGGER_CONFIG_VERSION);
	assert(status.applied_count == full.entry_count);
	assert(status.pending_count == 0U);

	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(env.setter_calls == full.entry_count * 2U);
	for (size_t i = 0U; i < full.entry_count; i++) {
		assert(entries_equal(&env.setter_trace[full.entry_count + i], &full.entries[i]));
	}
	assert_owners_idle(&env);
}

static void test_init_boot_failure_and_busy_remain_observable(void)
{
	struct linkr_debugger_config_snapshot full;
	struct fake_env env;
	struct linkr_debugger_config_service_status status;
	struct linkr_debugger_config_operation_report report;

	build_full_ordered(&full);

	reset_env(&env);
	script_store_ready(&env, &full);
	env.setter_fail_at = 0U;
	env.setter_failure_errno = TEST_ERR_REMOTE_IO;
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED);
	assert(env.setter_calls == 1U);
	assert_owners_idle(&env);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.available);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY);
	assert(status.failed_count == 1U);
	assert(status.failed_errno == TEST_ERR_REMOTE_IO);
	assert(status.failed_item ==
	       linkr_debugger_config_find_item(full.entries[0].domain,
				       full.entries[0].item_id));
	assert(status.items[catalog_index(status.failed_item)].apply_state ==
	       LINKR_DEBUGGER_CONFIG_APPLY_FAILED);
	assert(status.pending_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES - 1U);

	reset_env(&env);
	script_store_ready(&env, &full);
	env.capture_busy = true;
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_CAPTURE);
	assert(env.setter_calls == 0U);
	assert(env.flash_acquire_calls == 0U);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.available);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY);
	assert(status.snapshot_present);
	assert(status.saved_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(status.pending_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(status.applied_count == 0U);

	env.capture_busy = false;
	{
		struct linkr_debugger_config_save_request request;

		make_save_request(&request, false, true);
		assert(linkr_debugger_config_service_save(&request, &report) ==
		       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	}
	assert(env.setter_calls == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(report.applied_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.applied_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(status.pending_count == 0U);
	assert_owners_idle(&env);

	reset_env(&env);
	script_store_ready(&env, &full);
	env.flash_busy = true;
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_FLASH);
	assert(env.setter_calls == 0U);
	assert(env.capture_acquire_calls == 1U);
	assert(env.capture_release_calls == 1U);
	assert(env.flash_acquire_calls == 1U);
	assert_owners_idle(&env);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.pending_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
}

static void test_failed_reinit_preserves_initialized_state(void)
{
	struct fake_env env;
	struct linkr_debugger_config_service_ops ops;
	struct linkr_debugger_config_service_status status;
	void *null_fn = NULL;

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	build_ops(&env, &ops);
	memcpy((char *)&ops +
		offsetof(struct linkr_debugger_config_service_ops, store_save),
	       &null_fn, sizeof(null_fn));
	assert(linkr_debugger_config_service_init_with_ops(&ops, &env) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.available);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT);
}

static void make_save_request(struct linkr_debugger_config_save_request *request,
			      bool reverse, bool confirmed)
{
	memset(request, 0, sizeof(*request));
	request->item_count = linkr_debugger_config_item_count;
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		size_t pick = reverse ? linkr_debugger_config_item_count - i - 1U : i;

		request->item_ids[i] = linkr_debugger_config_items[pick].id;
	}
	request->confirmed = confirmed;
}

static void test_save_happy_23_items(void)
{
	struct fake_env env;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_operation_report report;
	struct linkr_debugger_config_service_status status;

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	make_save_request(&request, true, true);
	memset(&report, 0xa5, sizeof(report));
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(report.result == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(report.snapshot_version == LINKR_DEBUGGER_CONFIG_VERSION);
	assert(report.confirmation_count == 1U);
	assert(report.confirmation_items[0] ==
	       linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
				       LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID));
	assert(report.confirmation_items[1] == NULL);
	assert(report.applied_count == linkr_debugger_config_item_count);
	assert(report.pending_count == 0U);
	assert(report.failed_item == NULL);
	assert(env.store_save_calls == 1U);
	assert(env.store_clear_calls == 0U);
	assert(env.setter_calls == linkr_debugger_config_item_count);
	assert(env.control_snapshot_calls == 1U);
	assert(env.capture_acquire_calls == 1U);
	assert(env.capture_release_calls == 1U);
	assert(env.flash_acquire_calls == 1U);
	assert(env.flash_release_calls == 1U);
	assert_owners_idle(&env);

	assert(env.store_last_saved.entry_count == linkr_debugger_config_item_count);
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		const struct linkr_debugger_config_item_desc *item =
			&linkr_debugger_config_items[i];
		const struct linkr_debugger_config_entry *entry =
			&env.store_last_saved.entries[i];

		assert(entry->domain == item->domain);
		assert(entry->item_id == item->item_id);
		assert(entry->value == env.control_items[i].value);
	}

	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.available);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY);
	assert(status.snapshot_present);
	assert(status.snapshot_version == LINKR_DEBUGGER_CONFIG_VERSION);
	assert(status.saved_count == linkr_debugger_config_item_count);
	assert(status.applied_count == linkr_debugger_config_item_count);
	assert(status.pending_count == 0U);
	assert(status.failed_count == 0U);
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		assert(status.items[i].saved);
		assert(status.items[i].saved_value == env.control_items[i].value);
		assert(status.items[i].apply_state ==
		       LINKR_DEBUGGER_CONFIG_APPLY_APPLIED);
	}
}

static void test_save_validation_failures_have_no_side_effects(void)
{
	struct fake_env env;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_operation_report report;
	size_t baseline_store_calls;
	size_t baseline_control_calls;
	size_t baseline_owner_calls;

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	baseline_store_calls = env.store_save_calls;
	baseline_control_calls = env.control_snapshot_calls;
	baseline_owner_calls = env.capture_acquire_calls + env.flash_acquire_calls;

	assert(linkr_debugger_config_service_save(NULL, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	make_save_request(&request, false, true);
	assert(linkr_debugger_config_service_save(&request, NULL) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);

	memset(&request, 0, sizeof(request));
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION);
	assert(report.result == LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION);

	memset(&request, 0, sizeof(request));
	request.item_count = LINKR_DEBUGGER_CONFIG_MAX_ENTRIES + 1U;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);

	memset(&request, 0, sizeof(request));
	request.item_count = 1U;
	request.item_ids[0] = "power/not_a_rail";
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM);

	request.item_ids[0] = NULL;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);

	memset(&request, 0, sizeof(request));
	request.item_count = 2U;
	request.item_ids[0] = linkr_debugger_config_items[0].id;
	request.item_ids[1] = linkr_debugger_config_items[0].id;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_DUPLICATE_ITEM);

	assert(env.store_save_calls == baseline_store_calls);
	assert(env.control_snapshot_calls == baseline_control_calls);
	assert(env.capture_acquire_calls + env.flash_acquire_calls ==
	       baseline_owner_calls);
	assert_owners_idle(&env);
}

static void test_save_unavailable_and_capture_failure(void)
{
	struct fake_env env;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_operation_report report;
	size_t vin_index = catalog_index(linkr_debugger_config_find_item(
		LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID));

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);

	env.control_items[vin_index].available = false;
	memset(&request, 0, sizeof(request));
	request.item_count = 2U;
	request.item_ids[0] = linkr_debugger_config_items[0].id;
	request.item_ids[1] = linkr_debugger_config_items[vin_index].id;
	request.confirmed = true;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_ITEM_UNAVAILABLE);
	assert(env.store_save_calls == 0U);
	assert(env.setter_calls == 0U);
	assert_owners_idle(&env);

	env.control_items[vin_index].available = true;
	env.control_snapshot_result = TEST_ERR_IO;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_CONTROL_CAPTURE_FAILED);
	assert(env.store_save_calls == 0U);
	assert_owners_idle(&env);

	env.control_snapshot_result = 0;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(env.store_save_calls == 1U);
	assert(env.setter_calls == 2U);
}

static void test_save_confirmation_flow_preserves_prior_state(void)
{
	struct fake_env env;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_operation_report report;
	struct linkr_debugger_config_service_status status;
	size_t rail_index = catalog_index(linkr_debugger_config_find_item(
		LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID));

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	env.control_items[rail_index].value = LINKR_DEBUGGER_CONFIG_POWER_ON;

	memset(&request, 0, sizeof(request));
	request.item_count = 2U;
	request.item_ids[0] = linkr_debugger_config_items[rail_index].id;
	request.item_ids[1] = "switch/sd";
	request.confirmed = false;
	memset(&report, 0xa5, sizeof(report));
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED);
	assert(report.result == LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED);
	assert(report.snapshot_version == 0U);
	assert(report.confirmation_count == 1U);
	assert(report.confirmation_items[0] ==
	       linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
				       LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID));
	assert(env.store_save_calls == 0U);
	assert(env.setter_calls == 0U);
	assert_owners_idle(&env);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT);
	assert(!status.snapshot_present);
	assert(status.saved_count == 0U);

	request.confirmed = true;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(report.confirmation_count == 1U);
	assert(report.confirmation_items[0] ==
	       linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
				       LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID));
	assert(env.store_save_calls == 1U);
	assert(env.setter_calls == 2U);
	assert(env.store_last_saved.entry_count == 2U);
	assert(env.store_last_saved.entries[0].domain ==
	       LINKR_DEBUGGER_CONFIG_DOMAIN_POWER);
	assert(env.store_last_saved.entries[1].domain ==
	       LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH);

	env.store_save_result = LINKR_DEBUGGER_CONFIG_STORE_STORAGE_ERROR;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_STORAGE_ERROR);
	assert(env.store_save_calls == 2U);
	assert_owners_idle(&env);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY);
	assert(status.saved_count == 2U);
	assert(status.applied_count == 2U);
	assert(status.items[rail_index].saved);
	assert(status.items[rail_index].saved_value ==
	       LINKR_DEBUGGER_CONFIG_POWER_ON);
	assert(status.items[rail_index].apply_state ==
	       LINKR_DEBUGGER_CONFIG_APPLY_APPLIED);

	env.store_save_result = LINKR_DEBUGGER_CONFIG_STORE_BACKEND_UNAVAILABLE;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.saved_count == 2U);
}

static void test_save_busy_aborts_before_control_and_store(void)
{
	struct fake_env env;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_operation_report report;
	size_t control_baseline;

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	make_save_request(&request, false, true);
	control_baseline = env.control_snapshot_calls;

	env.capture_busy = true;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_CAPTURE);
	assert(report.result == LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_CAPTURE);
	assert(env.flash_acquire_calls == 0U);
	assert(env.control_snapshot_calls == control_baseline);
	assert(env.store_save_calls == 0U);

	env.capture_busy = false;
	env.flash_busy = true;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_FLASH);
	assert(env.capture_acquire_calls == 2U);
	assert(env.capture_release_calls == 1U);
	assert(env.control_snapshot_calls == control_baseline);
	assert(env.store_save_calls == 0U);
	assert_owners_idle(&env);

	env.flash_busy = false;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(env.store_save_calls == 1U);
	assert(env.setter_calls == linkr_debugger_config_item_count);
}

static void test_save_partial_failure_reports_pending_and_retry(void)
{
	struct fake_env env;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_operation_report report;
	struct linkr_debugger_config_service_status status;

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	make_save_request(&request, false, true);

	/* All fifteen captured GPIOs are inputs and replay first in catalog
	 * order (GP7..GP20, GP29), so replay position five is GP12. */
	env.setter_fail_at = 5U;
	env.setter_failure_errno = TEST_ERR_IO;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED);
	assert(env.store_save_calls == 1U);
	assert(env.setter_calls == 6U);
	assert(report.result == LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED);
	assert(report.applied_count == 5U);
	assert(report.pending_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES - 5U);
	assert(report.pending_items[0] == report.failed_item);
	assert(report.failed_item ==
	       linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO,
				       12U));
	assert(report.failed_errno == TEST_ERR_IO);
	assert_owners_idle(&env);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.snapshot_present);
	assert(status.saved_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(status.applied_count == 5U);
	assert(status.failed_count == 1U);
	assert(status.pending_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES - 6U);
	assert(status.failed_errno == TEST_ERR_IO);

	env.setter_fail_at = SETTER_NEVER;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(env.store_save_calls == 2U);
	assert(report.result == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(report.applied_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(report.pending_count == 0U);
	assert(report.failed_item == NULL);
	assert(report.failed_errno == 0);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.applied_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(status.pending_count == 0U);
	assert(status.failed_count == 0U);
	assert(status.failed_item == NULL);
	assert(status.failed_errno == 0);
	assert_owners_idle(&env);
}

static void test_clear_lifecycle_and_failure_preservation(void)
{
	struct fake_env env;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_operation_report report;
	struct linkr_debugger_config_service_status status;
	size_t setter_baseline;

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	make_save_request(&request, false, true);
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	setter_baseline = env.setter_calls;
	assert(setter_baseline == linkr_debugger_config_item_count);

	assert(linkr_debugger_config_service_clear() ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(env.store_clear_calls == 1U);
	assert(env.setter_calls == setter_baseline);
	assert_owners_idle(&env);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.available);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT);
	assert(!status.snapshot_present);
	assert(status.snapshot_version == 0U);
	assert(status.saved_count == 0U);
	assert(status.applied_count == 0U);
	assert(status.pending_count == 0U);
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		assert(!status.items[i].saved);
		assert(status.items[i].apply_state ==
		       LINKR_DEBUGGER_CONFIG_APPLY_NOT_SAVED);
	}

	assert(linkr_debugger_config_service_clear() ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(env.store_clear_calls == 2U);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT);

	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	env.store_clear_result = LINKR_DEBUGGER_CONFIG_STORE_STORAGE_ERROR;
	assert(linkr_debugger_config_service_clear() ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_STORAGE_ERROR);
	assert_owners_idle(&env);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY);
	assert(status.saved_count == linkr_debugger_config_item_count);

	env.store_clear_result = LINKR_DEBUGGER_CONFIG_STORE_OK;
	env.capture_busy = true;
	assert(linkr_debugger_config_service_clear() ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_CAPTURE);
	env.capture_busy = false;
	env.flash_busy = true;
	assert(linkr_debugger_config_service_clear() ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_FLASH);
	assert(env.setter_calls == linkr_debugger_config_item_count * 2U);
	assert_owners_idle(&env);
	env.flash_busy = false;
	assert(linkr_debugger_config_service_clear() ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT);
}

static void test_status_merges_current_without_side_effects(void)
{
	struct fake_env env;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_operation_report report;
	struct linkr_debugger_config_service_status status;
	size_t rail_index = catalog_index(linkr_debugger_config_find_item(
		LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID));
	size_t vin_index = catalog_index(linkr_debugger_config_find_item(
		LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID));
	size_t capture_calls;
	size_t flash_calls;
	size_t store_save_calls;
	size_t store_clear_calls;

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	make_save_request(&request, false, true);
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);

	env.control_items[rail_index].value = LINKR_DEBUGGER_CONFIG_POWER_ON;
	env.control_items[vin_index].available = false;
	capture_calls = env.capture_acquire_calls;
	flash_calls = env.flash_acquire_calls;
	store_save_calls = env.store_save_calls;
	store_clear_calls = env.store_clear_calls;

	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(env.control_snapshot_calls == 2U);
	assert(env.capture_acquire_calls == capture_calls);
	assert(env.flash_acquire_calls == flash_calls);
	assert(env.store_save_calls == store_save_calls);
	assert(env.store_clear_calls == store_clear_calls);
	assert(status.available);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY);
	assert(status.item_count == linkr_debugger_config_item_count);
	assert(status.items[rail_index].current_available);
	assert(status.items[rail_index].current_value ==
	       LINKR_DEBUGGER_CONFIG_POWER_ON);
	assert(status.items[rail_index].current_requires_confirmation);
	assert(!status.items[vin_index].current_available);
	assert(status.items[rail_index].saved);
	assert(status.items[rail_index].saved_value ==
	       LINKR_DEBUGGER_CONFIG_POWER_OFF);
	assert(!status.items[rail_index].saved_requires_confirmation);
	assert(status.items[rail_index].apply_state ==
	       LINKR_DEBUGGER_CONFIG_APPLY_APPLIED);

	env.control_snapshot_result = TEST_ERR_IO;
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		assert(!status.items[i].current_available);
	}
	assert(status.saved_count == linkr_debugger_config_item_count);
}

static void test_production_init_delegates_to_frozen_ops(void)
{
	struct fake_env env;
	struct linkr_debugger_config_service_status status;

	reset_env(&env);
	script_store_absent(&env);
	production_env = &env;
	assert(linkr_debugger_config_service_init() ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(env.store_status_calls == 1U);
	assert(env.store_snapshot_calls == 0U);
	assert(env.setter_calls == 0U);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.available);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT);
	assert_owners_idle(&env);
}

static void test_owner_release_failure_is_observable(void)
{
	struct fake_env env;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_operation_report report;

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	make_save_request(&request, false, true);

	env.capture_release_ok = false;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(linkr_debugger_config_service_capture_release_failures == 1U);
	assert(linkr_debugger_config_service_flash_release_failures == 0U);
	assert(env.store_save_calls == 1U);

	env.capture_release_ok = true;
	env.flash_release_ok = false;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(linkr_debugger_config_service_capture_release_failures == 1U);
	assert(linkr_debugger_config_service_flash_release_failures == 1U);
	assert(env.store_save_calls == 2U);
	assert(env.capture_held == 0U);
	assert(env.flash_held == 0U);
}

struct blocked_call_args {
	struct fake_env *env;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_operation_report report;
	struct linkr_debugger_config_service_status status;
	enum linkr_debugger_config_service_result save_result;
	enum linkr_debugger_config_service_result status_result;
	enum linkr_debugger_config_service_result clear_result;
	atomic_bool status_done;
	atomic_bool clear_done;
};

static void *blocked_save_thread(void *argument)
{
	struct blocked_call_args *args = argument;

	args->save_result =
		linkr_debugger_config_service_save(&args->request, &args->report);
	return NULL;
}

static void *blocked_status_thread(void *argument)
{
	struct blocked_call_args *args = argument;

	args->status_result =
		linkr_debugger_config_service_status_get(&args->status);
	atomic_store_explicit(&args->status_done, true, memory_order_release);
	return NULL;
}

static void *blocked_clear_thread(void *argument)
{
	struct blocked_call_args *args = argument;

	args->clear_result = linkr_debugger_config_service_clear();
	atomic_store_explicit(&args->clear_done, true, memory_order_release);
	return NULL;
}

static void wait_for_save_entry(struct fake_env *env)
{
	assert(pthread_mutex_lock(&blocking_mutex) == 0);
	for (size_t i = 0U; i < BLOCK_POLL_ROUNDS && !env->save_entered; i++) {
		struct timespec deadline;

		assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
		deadline.tv_nsec += 10000000L;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000L;
		}
		(void)pthread_cond_timedwait(&blocking_cond, &blocking_mutex,
					     &deadline);
	}
	assert(env->save_entered);
	assert(pthread_mutex_unlock(&blocking_mutex) == 0);
}

static void test_blocked_store_save_holds_no_control_lock(void)
{
	struct fake_env env;
	struct blocked_call_args args;
	pthread_t save_thread;
	pthread_t status_thread;
	pthread_t clear_thread;
	struct timespec pause = { 0, 20000000L };
	struct linkr_debugger_config_service_status final_status;

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	env.block_save = true;

	memset(&args, 0, sizeof(args));
	args.env = &env;
	atomic_init(&args.status_done, false);
	atomic_init(&args.clear_done, false);
	args.request.item_count = 2U;
	args.request.item_ids[0] = "power/12v_out";
	args.request.item_ids[1] = "switch/sd";
	args.request.confirmed = false;

	assert(pthread_create(&save_thread, NULL, blocked_save_thread, &args) == 0);
	wait_for_save_entry(&env);
	assert(env.control_snapshot_returned);
	assert(env.control_lock_depth == 0U);
	assert(env.control_lock_max_depth == 1U);
	assert(env.store_save_depth == 1U);

	assert(pthread_create(&status_thread, NULL, blocked_status_thread, &args) ==
	       0);
	assert(pthread_create(&clear_thread, NULL, blocked_clear_thread, &args) ==
	       0);
	for (size_t i = 0U; i < 5U; i++) {
		(void)nanosleep(&pause, NULL);
		assert(!atomic_load_explicit(&args.status_done, memory_order_acquire));
		assert(!atomic_load_explicit(&args.clear_done, memory_order_acquire));
	}

	assert(pthread_mutex_lock(&blocking_mutex) == 0);
	env.save_release = true;
	assert(pthread_cond_broadcast(&blocking_cond) == 0);
	assert(pthread_mutex_unlock(&blocking_mutex) == 0);

	assert(pthread_join(save_thread, NULL) == 0);
	assert(pthread_join(status_thread, NULL) == 0);
	assert(pthread_join(clear_thread, NULL) == 0);
	assert(args.save_result == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(args.status_result == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(args.clear_result == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(env.store_save_calls == 1U);
	assert(env.store_clear_calls == 1U);
	assert(env.store_save_max_depth == 1U);
	assert(env.setter_calls == 2U);
	assert_owners_idle(&env);

	assert(linkr_debugger_config_service_status_get(&final_status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(final_status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT);
	assert(final_status.saved_count == 0U);
}

static void test_status_merge_reordered_rows(void)
{
	struct fake_env env;
	struct linkr_debugger_config_service_status status;
	size_t rail_index = catalog_index(linkr_debugger_config_find_item(
		LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID));
	size_t vin_index = catalog_index(linkr_debugger_config_find_item(
		LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID));

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	env.control_items[rail_index].value = LINKR_DEBUGGER_CONFIG_POWER_ON;
	env.control_items[vin_index].available = false;
	for (size_t i = 0U; i < linkr_debugger_config_item_count / 2U; i++) {
		struct linkr_debugger_control_item_state swap = env.control_items[i];

		env.control_items[i] =
			env.control_items[linkr_debugger_config_item_count - i - 1U];
		env.control_items[linkr_debugger_config_item_count - i - 1U] = swap;
	}
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		const struct linkr_debugger_config_item_desc *item =
			&linkr_debugger_config_items[i];

		assert(status.items[i].current_available == (i != vin_index));
		if (i == rail_index) {
			assert(status.items[i].current_value ==
			       LINKR_DEBUGGER_CONFIG_POWER_ON);
		} else if (item->domain != LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH) {
			assert(status.items[i].current_value == 0U);
		}
		if (item->domain == LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH &&
		    item->item_id == LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID) {
			assert(status.items[i].current_requires_confirmation);
		}
	}
	assert(status.items[rail_index].current_requires_confirmation);
	assert(!status.items[vin_index].current_available);
}

static void assert_current_fields_unavailable(
	const struct linkr_debugger_config_service_status *status)
{
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		assert(!status->items[i].current_available);
		assert(status->items[i].current_value == 0U);
		assert(!status->items[i].current_requires_confirmation);
	}
}

static void test_status_merge_rejects_malformed_rows(void)
{
	struct fake_env env;
	struct linkr_debugger_config_service_status status;

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);

	env.control_items[1].domain = env.control_items[0].domain;
	env.control_items[1].item_id = env.control_items[0].item_id;
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert_current_fields_unavailable(&status);

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	env.control_items[3].domain = 9U;
	env.control_items[3].item_id = 9U;
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert_current_fields_unavailable(&status);

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	env.control_row_count_override = linkr_debugger_config_item_count - 1U;
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert_current_fields_unavailable(&status);

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	env.control_row_count_override = LINKR_DEBUGGER_CONFIG_MAX_ENTRIES + 1U;
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert_current_fields_unavailable(&status);
}

static void test_invalid_enum_values_use_deterministic_fallback(void)
{
	struct fake_env env;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_operation_report report;
	struct linkr_debugger_config_service_status status;

	reset_env(&env);
	script_store_absent(&env);
	env.store_status.reason = (enum linkr_debugger_config_store_reason)99;
	assert(init_fresh(&env) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(!status.available);
	assert(status.reason ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_REASON_BACKEND_UNAVAILABLE);

	reset_env(&env);
	script_store_absent(&env);
	env.store_status_result = (enum linkr_debugger_config_store_result)200;
	assert(init_fresh(&env) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.reason ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_REASON_BACKEND_UNAVAILABLE);

	reset_env(&env);
	script_store_absent(&env);
	assert(init_fresh(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	make_save_request(&request, false, true);
	env.store_save_result = (enum linkr_debugger_config_store_result)77;
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert_owners_idle(&env);
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT);
	assert(status.saved_count == 0U);
}

static enum linkr_debugger_config_service_result init_with_frame_ops(
	struct fake_env *env)
{
	struct linkr_debugger_config_service_ops ops;

	build_ops(env, &ops);
	return linkr_debugger_config_service_init_with_ops(&ops, env);
}

static void clobber_stack_frames(void)
{
	volatile uint8_t cover[512];

	for (size_t i = 0U; i < sizeof(cover); i++) {
		cover[i] = (uint8_t)(0xa5U + i);
	}
}

static void test_stack_local_ops_lifetime_is_copied(void)
{
	struct fake_env env;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_operation_report report;
	struct linkr_debugger_config_service_status status;

	reset_env(&env);
	script_store_absent(&env);
	assert(init_with_frame_ops(&env) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	clobber_stack_frames();
	make_save_request(&request, false, true);
	assert(linkr_debugger_config_service_save(&request, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	clobber_stack_frames();
	assert(linkr_debugger_config_service_status_get(&status) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY);
	assert(status.saved_count == linkr_debugger_config_item_count);
	assert_owners_idle(&env);
}

int main(void)
{
	test_ops_validation_and_pre_init_contract();
	test_init_maps_every_store_state();
	test_init_replays_full_snapshot_including_dangerous();
	test_init_replays_snapshot_on_every_boot();
	test_init_boot_failure_and_busy_remain_observable();
	test_failed_reinit_preserves_initialized_state();
	test_save_happy_23_items();
	test_save_validation_failures_have_no_side_effects();
	test_save_unavailable_and_capture_failure();
	test_save_confirmation_flow_preserves_prior_state();
	test_save_busy_aborts_before_control_and_store();
	test_save_partial_failure_reports_pending_and_retry();
	test_clear_lifecycle_and_failure_preservation();
	test_status_merges_current_without_side_effects();
	test_status_merge_reordered_rows();
	test_status_merge_rejects_malformed_rows();
	test_invalid_enum_values_use_deterministic_fallback();
	test_stack_local_ops_lifetime_is_copied();
	test_production_init_delegates_to_frozen_ops();
	test_owner_release_failure_is_observable();
	test_blocked_store_save_holds_no_control_lock();
	printf("linkr_debugger_config_service: states=6 save-applies=23/23 busy=8 "
	       "merge=keyed fallback=checked ops=copied blocking=verified passed\n");
	return 0;
}
