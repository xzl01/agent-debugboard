#include "linkr_debugger_bootsel_button.h"

#include <assert.h>
#include <stdio.h>

static const struct linkr_debugger_bootsel_button_config config = {
	.debounce_samples = 3U,
	.long_press_ms = 3000U,
};

static enum linkr_debugger_bootsel_button_action step(
	struct linkr_debugger_bootsel_button_state *state,
	enum linkr_debugger_bootsel_button_sample sample,
	int64_t now_ms)
{
	return linkr_debugger_bootsel_button_state_step(state, &config,
						       sample, now_ms);
}

static void confirm_press(struct linkr_debugger_bootsel_button_state *state,
			  int64_t start_ms)
{
	assert(step(state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED,
		    start_ms) == LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(step(state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED,
		    start_ms + 20) == LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(step(state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED,
		    start_ms + 40) == LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(state->id == LINKR_DEBUGGER_BOOTSEL_STATE_PRESSED);
}

static void test_bounce(void)
{
	struct linkr_debugger_bootsel_button_state state;

	linkr_debugger_bootsel_button_state_init(&state);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED, 0) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(state.id == LINKR_DEBUGGER_BOOTSEL_STATE_DEBOUNCE_PRESS);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_RELEASED, 20) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(state.id == LINKR_DEBUGGER_BOOTSEL_STATE_RELEASED);

	confirm_press(&state, 40);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_RELEASED, 100) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_RELEASED, 120) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(state.id == LINKR_DEBUGGER_BOOTSEL_STATE_PRESSED);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED, 140) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(state.id == LINKR_DEBUGGER_BOOTSEL_STATE_PRESSED);
}

static void test_short_press(void)
{
	struct linkr_debugger_bootsel_button_state state;

	linkr_debugger_bootsel_button_state_init(&state);
	confirm_press(&state, 1000);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_RELEASED, 1100) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_RELEASED, 1120) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_RELEASED, 1140) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(state.id == LINKR_DEBUGGER_BOOTSEL_STATE_RELEASED);
}

static void test_long_press_stays_armed_while_held(void)
{
	struct linkr_debugger_bootsel_button_state state;

	linkr_debugger_bootsel_button_state_init(&state);
	confirm_press(&state, 0);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED, 2999) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(state.id == LINKR_DEBUGGER_BOOTSEL_STATE_PRESSED);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED, 3000) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(state.id == LINKR_DEBUGGER_BOOTSEL_STATE_WAIT_RELEASE);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED, 10000) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(state.id == LINKR_DEBUGGER_BOOTSEL_STATE_WAIT_RELEASE);
}

static void test_wait_release_debounce(void)
{
	struct linkr_debugger_bootsel_button_state state;

	linkr_debugger_bootsel_button_state_init(&state);
	confirm_press(&state, 0);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED, 3000) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_RELEASED, 3020) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_RELEASED, 3040) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED, 3060) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_RELEASED, 3080) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_RELEASED, 3100) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_RELEASED, 3120) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_REBOOT);
	assert(state.id == LINKR_DEBUGGER_BOOTSEL_STATE_RELEASED);
}

static void test_flash_busy_is_not_a_sample(void)
{
	struct linkr_debugger_bootsel_button_state state;

	linkr_debugger_bootsel_button_state_init(&state);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED, 0) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_UNAVAILABLE, 2000) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(state.id == LINKR_DEBUGGER_BOOTSEL_STATE_RELEASED);
	assert(state.consecutive_samples == 0U);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED, 2020) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED, 2040) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(state.id == LINKR_DEBUGGER_BOOTSEL_STATE_DEBOUNCE_PRESS);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_UNAVAILABLE, 5000) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(state.id == LINKR_DEBUGGER_BOOTSEL_STATE_RELEASED);
	assert(step(&state, LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED, 5020) ==
	       LINKR_DEBUGGER_BOOTSEL_ACTION_NONE);
	assert(state.id == LINKR_DEBUGGER_BOOTSEL_STATE_DEBOUNCE_PRESS);
}

int main(void)
{
	test_bounce();
	test_short_press();
	test_long_press_stays_armed_while_held();
	test_wait_release_debounce();
	test_flash_busy_is_not_a_sample();
	puts("linkr_debugger_bootsel_button tests passed");
	return 0;
}
