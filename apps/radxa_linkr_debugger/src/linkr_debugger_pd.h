/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_PD_H_
#define RADXA_LINKR_DEBUGGER_PD_H_

#include <stdbool.h>
#include <stdint.h>

#define LINKR_DEBUGGER_PD_VOLTAGE_5V 5000U
#define LINKR_DEBUGGER_PD_VOLTAGE_9V 9000U
#define LINKR_DEBUGGER_PD_VOLTAGE_12V 12000U
#define LINKR_DEBUGGER_PD_VOLTAGE_15V 15000U
#define LINKR_DEBUGGER_PD_VOLTAGE_20V 20000U

#define LINKR_DEBUGGER_PD_PPS_MIN_MV 3300U
#define LINKR_DEBUGGER_PD_PPS_MAX_MV 20000U
#define LINKR_DEBUGGER_PD_PPS_STEP_MV 100U

enum linkr_debugger_pd_mode {
	LINKR_DEBUGGER_PD_MODE_FIXED = 0,
	LINKR_DEBUGGER_PD_MODE_PPS = 1,
};

enum linkr_debugger_pd_protocol {
	LINKR_DEBUGGER_PD_PROTOCOL_NONE = 0,
	LINKR_DEBUGGER_PD_PROTOCOL_BC,
	LINKR_DEBUGGER_PD_PROTOCOL_QC2,
	LINKR_DEBUGGER_PD_PROTOCOL_QC3,
	LINKR_DEBUGGER_PD_PROTOCOL_PD,
	LINKR_DEBUGGER_PD_PROTOCOL_EPR,
};

#define LINKR_DEBUGGER_PD_MAX_SOURCE_PROFILES 10U

struct linkr_debugger_pd_fixed_profile {
	uint16_t voltage_mv;
	uint16_t max_current_ma;
};

struct linkr_debugger_pd_programmable_range {
	uint16_t min_voltage_mv;
	uint16_t max_voltage_mv;
	uint16_t step_mv;
	uint16_t max_current_ma;
	uint16_t max_power_w;
};

struct linkr_debugger_pd_status {
	bool available;
	enum linkr_debugger_pd_mode mode;
	enum linkr_debugger_pd_protocol protocol;
	bool bc;
	bool qc2;
	bool qc3;
	bool pd_active;
	bool epr;
	bool epr_exist;
	bool avs;
	uint8_t gear;
	uint16_t voltage_mv;
	uint16_t pps_mv;
	uint16_t max_current_ma;
	bool capabilities_valid;
	uint8_t fixed_count;
	uint8_t pps_count;
	uint8_t avs_count;
	struct linkr_debugger_pd_fixed_profile fixed[LINKR_DEBUGGER_PD_MAX_SOURCE_PROFILES];
	struct linkr_debugger_pd_programmable_range pps[LINKR_DEBUGGER_PD_MAX_SOURCE_PROFILES];
	struct linkr_debugger_pd_programmable_range avs_ranges[LINKR_DEBUGGER_PD_MAX_SOURCE_PROFILES];
	int last_error;
};

/* Explicitly initialize the deferred CH224A device, discover initial state,
 * and start the single dedicated ~100 ms input-gate monitor thread.
 */
int linkr_debugger_pd_init(void);
bool linkr_debugger_pd_available(void);
int linkr_debugger_pd_set_fixed(uint16_t voltage_mv);
int linkr_debugger_pd_set_pps(uint16_t voltage_mv);
/* Probe/read short status registers and copy only unexpired cached
 * capabilities; never read source capability registers (0x60..0x8f). */
int linkr_debugger_pd_get_status_short(struct linkr_debugger_pd_status *status);
/* Refresh missing or expired capabilities (5 s TTL). ADC gate or I2C errors
 * make the source unavailable and start the 5 s probe backoff; a complete but
 * invalid PDO window only starts capability backoff. */
int linkr_debugger_pd_get_status(struct linkr_debugger_pd_status *status);
int linkr_debugger_pd_voltage_to_gear(uint16_t voltage_mv, uint8_t *gear);
int linkr_debugger_pd_gear_to_voltage(uint8_t gear, uint16_t *voltage_mv);

#endif /* RADXA_LINKR_DEBUGGER_PD_H_ */
