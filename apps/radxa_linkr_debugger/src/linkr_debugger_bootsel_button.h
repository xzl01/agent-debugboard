#ifndef RADXA_LINKR_DEBUGGER_BOOTSEL_BUTTON_H_
#define RADXA_LINKR_DEBUGGER_BOOTSEL_BUTTON_H_

#include <stdint.h>

enum linkr_debugger_bootsel_button_sample {
	LINKR_DEBUGGER_BOOTSEL_SAMPLE_UNAVAILABLE = 0,
	LINKR_DEBUGGER_BOOTSEL_SAMPLE_RELEASED,
	LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED,
};

enum linkr_debugger_bootsel_button_state_id {
	LINKR_DEBUGGER_BOOTSEL_STATE_RELEASED = 0,
	LINKR_DEBUGGER_BOOTSEL_STATE_DEBOUNCE_PRESS,
	LINKR_DEBUGGER_BOOTSEL_STATE_PRESSED,
	LINKR_DEBUGGER_BOOTSEL_STATE_WAIT_RELEASE,
};

enum linkr_debugger_bootsel_button_action {
	LINKR_DEBUGGER_BOOTSEL_ACTION_NONE = 0,
	LINKR_DEBUGGER_BOOTSEL_ACTION_REBOOT,
};

struct linkr_debugger_bootsel_button_config {
	uint32_t debounce_samples;
	uint32_t long_press_ms;
};

struct linkr_debugger_bootsel_button_state {
	enum linkr_debugger_bootsel_button_state_id id;
	uint32_t consecutive_samples;
	int64_t pressed_since_ms;
};

void linkr_debugger_bootsel_button_state_init(
	struct linkr_debugger_bootsel_button_state *state);

enum linkr_debugger_bootsel_button_action
linkr_debugger_bootsel_button_state_step(
	struct linkr_debugger_bootsel_button_state *state,
	const struct linkr_debugger_bootsel_button_config *config,
	enum linkr_debugger_bootsel_button_sample sample,
	int64_t now_ms);

#endif
