/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 *
 * PIO-based I2C master for the RP2350.
 *
 * The x1.2 board routes the I2C bus to GPIO20 (SCL) and GPIO21 (SDA); the
 * schematic nets are crossed relative to the RP2350 I2C0 pin functions.
 * The pico-examples program normally requires SCL = SDA + 1, so the two
 * wait instructions are encoded as absolute GPIO waits on SCL (GPIO20)
 * instead of relative pin waits. SDA is sampled on GPIO21.
 *
 * The program and byte-level sequencing are the pico-examples pio/i2c
 * implementation (BSD-3-Clause).
 */

#define DT_DRV_COMPAT raspberrypi_pico_pio_i2c

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>

#include <errno.h>
#include <stdint.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(i2c_rpi_pico_pio, LOG_LEVEL_INF);

#define PIO_I2C_ICOUNT_LSB 10U
#define PIO_I2C_FINAL_LSB  9U
#define PIO_I2C_DATA_LSB   1U
#define PIO_I2C_NAK_LSB    0U
#define PIO_I2C_CLOCKS_PER_BYTE 9U
#define PIO_I2C_MSG_OVERHEAD_CLOCKS 2U
#define PIO_I2C_BUS_IDLE_CLOCKS 64U
#define PIO_I2C_INIT_TOTAL_CLOCKS 448U
#define PIO_I2C_INIT_SETTLE_CLOCKS 64U
#define PIO_I2C_RECOVERY_COOLDOWN_CLOCKS 512U
#define PIO_I2C_TIMEOUT_MARGIN 2U
#define PIO_I2C_MSEC_PER_SEC 1000U
#define PIO_I2C_USEC_PER_SEC 1000000U
#define PIO_I2C_TIMEOUT_MIN_MSEC 100U
#define PIO_I2C_TIMEOUT_MAX_MSEC 5000U
#define PIO_I2C_INIT_MIN_MSEC 500U
#define PIO_I2C_RECOVERY_COOLDOWN_MIN_MSEC 1000U
#define PIO_I2C_LOCK_MIN_MSEC 500U
#define PIO_I2C_STABLE_SAMPLES 8U
#define PIO_I2C_STABLE_MIN_USEC 5000U
#define PIO_I2C_IDLE_SAMPLES 2U
#define PIO_I2C_IDLE_SAMPLE_MIN_USEC 5U
#define PIO_I2C_SAMPLE_MAX_USEC 1000U
#define PIO_I2C_CLKDIV_SCALE 256U
#define PIO_I2C_CLKDIV_MAX 65536U
#define PIO_I2C_MAX_REQUEST_HZ 400000U
#define PIO_I2C_FAST_TLOW_MIN_NS 1300U
#define PIO_I2C_FAST_THIGH_MIN_NS 600U
#define PIO_I2C_FAST_TSU_DAT_MIN_NS 100U
#define PIO_I2C_FAST_TVD_DAT_MAX_NS 900U
#define PIO_I2C_FAST_TVD_ACK_MAX_NS 900U
#define PIO_I2C_FAST_TSU_STA_MIN_NS 600U
#define PIO_I2C_FAST_THD_STA_MIN_NS 600U
#define PIO_I2C_FAST_TSU_STO_MIN_NS 600U
#define PIO_I2C_FAST_TBUF_MIN_NS 1300U
#define PIO_I2C_NSEC_PER_SEC 1000000000ULL

/* Keep every timing value derived from the machine words used below. With one
 * optional side-set bit, bits 10:8 are the delay and every instruction also
 * consumes its base cycle.
 */
#define PIO_I2C_INSTRUCTION_CYCLES(instruction) \
	(1U + (((instruction) >> 8U) & 0x7U))
#define PIO_I2C_INSTR_SET_X_7         0xe027U
#define PIO_I2C_INSTR_DATA_OUT        0x6781U
#define PIO_I2C_INSTR_DATA_LOW_HOLD   0xb142U
#define PIO_I2C_INSTR_DATA_HIGH       0xba42U
#define PIO_I2C_INSTR_DATA_WAIT       0x2494U
#define PIO_I2C_INSTR_DATA_IN         0x4701U
#define PIO_I2C_INSTR_DATA_FALL       0x1743U
#define PIO_I2C_INSTR_ACK_LOW_HOLD    0xb242U
#define PIO_I2C_INSTR_ACK_HIGH        0xbf42U
#define PIO_I2C_INSTR_ACK_WAIT        0x2794U
#define PIO_I2C_INSTR_ACK_FALL        0x13c0U
#define PIO_I2C_INSTR_PARSER_OUT_X    0x6026U
#define PIO_I2C_INSTR_PARSER_OUT_Y    0x6041U
#define PIO_I2C_INSTR_PARSER_JMP_DATA 0x0022U
#define PIO_I2C_INSTR_PARSER_OUT_NULL 0x6060U
#define PIO_I2C_INSTR_OUT_EXEC        0x60f0U
#define PIO_I2C_INSTR_EXEC_LOOP       0x0052U

/* pioasm output for the set_scl_sda instruction table (executed via OUT EXEC). */
#define PIO_I2C_SC0_SD0 0xf780U
#define PIO_I2C_SC0_SD1 0xf781U
#define PIO_I2C_SC1_SD0 0xff80U
#define PIO_I2C_SC1_SD1 0xff81U

#define PIO_I2C_DATA_TLOW_CYCLES \
	(PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_DATA_FALL) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_DATA_OUT) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_DATA_LOW_HOLD))
#define PIO_I2C_DATA_THIGH_CYCLES \
	(PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_DATA_HIGH) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_DATA_WAIT) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_DATA_IN))
#define PIO_I2C_DATA_SETUP_CYCLES \
	(PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_DATA_OUT) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_DATA_LOW_HOLD))
#define PIO_I2C_DATA_VALID_CYCLES \
	PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_DATA_FALL)

/* Successful ACK takes PC 13 -> 14 -> 15 -> 16 -> 2 -> 3 -> 4 before PC 5
 * releases SCL. Derive that complete low phase from those exact words.
 */
#define PIO_I2C_ACK_TLOW_CYCLES \
	(PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_ACK_FALL) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_PARSER_OUT_X) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_PARSER_OUT_Y) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_PARSER_JMP_DATA) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_SET_X_7) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_DATA_OUT) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_DATA_LOW_HOLD))
#define PIO_I2C_ACK_THIGH_CYCLES \
	(PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_ACK_HIGH) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_ACK_WAIT))
#define PIO_I2C_ACK_VALID_CYCLES \
	PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_DATA_FALL)
#define PIO_I2C_CYCLES_PER_BIT \
	(PIO_I2C_DATA_TLOW_CYCLES + PIO_I2C_DATA_THIGH_CYCLES)

/* A raw level consumes its eight-cycle SET plus OUT EXEC and the loop jump. */
#define PIO_I2C_ESCAPE_LEVEL_CYCLES \
	(PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_SC0_SD0) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_OUT_EXEC) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_EXEC_LOOP))
#define PIO_I2C_START_HOLD_CYCLES PIO_I2C_ESCAPE_LEVEL_CYCLES
#define PIO_I2C_REPSTART_SETUP_CYCLES PIO_I2C_ESCAPE_LEVEL_CYCLES
#define PIO_I2C_STOP_SETUP_CYCLES PIO_I2C_ESCAPE_LEVEL_CYCLES

/* OUT EXEC and its loop jump add two SM cycles to each eight-cycle raw level.
 * Recovery duplicates every low/high level, so its SCL period is 40 cycles.
 */
#define PIO_I2C_ESCAPE_SETUP_CYCLES \
	(PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_PARSER_OUT_X) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_PARSER_OUT_Y) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_PARSER_JMP_DATA) + \
	 PIO_I2C_INSTRUCTION_CYCLES(PIO_I2C_INSTR_PARSER_OUT_NULL))
#define PIO_I2C_RECOVERY_INSTRUCTION_COUNT 41U
#define PIO_I2C_RECOVERY_CYCLES_PER_PULSE (4U * PIO_I2C_ESCAPE_LEVEL_CYCLES)
#define PIO_I2C_RECOVERY_STOP_TLOW_CYCLES (2U * PIO_I2C_ESCAPE_LEVEL_CYCLES)
#define PIO_I2C_RECOVERY_STOP_SETUP_CYCLES (2U * PIO_I2C_ESCAPE_LEVEL_CYCLES)
#define PIO_I2C_RECOVERY_SM_CYCLES \
	(PIO_I2C_ESCAPE_SETUP_CYCLES + \
	 PIO_I2C_RECOVERY_INSTRUCTION_COUNT * PIO_I2C_ESCAPE_LEVEL_CYCLES)
#define PIO_I2C_RECOVERY_TRANSFER_CLOCKS \
	DIV_ROUND_UP(PIO_I2C_RECOVERY_SM_CYCLES, PIO_I2C_CYCLES_PER_BIT)
#define PIO_I2C_IDLE_CONFIRM_CLOCKS \
	DIV_ROUND_UP(PIO_I2C_IDLE_SAMPLE_MIN_USEC * PIO_I2C_MAX_REQUEST_HZ, \
		     PIO_I2C_USEC_PER_SEC)
#define PIO_I2C_RECOVERY_CLOCKS \
	(PIO_I2C_RECOVERY_TRANSFER_CLOCKS + PIO_I2C_IDLE_CONFIRM_CLOCKS)
#define PIO_I2C_STOP_INSTRUCTION_COUNT 3U
#define PIO_I2C_STOP_SM_CYCLES \
	(PIO_I2C_ESCAPE_SETUP_CYCLES + \
	 PIO_I2C_STOP_INSTRUCTION_COUNT * PIO_I2C_ESCAPE_LEVEL_CYCLES)
#define PIO_I2C_STOP_TRANSFER_CLOCKS \
	DIV_ROUND_UP(PIO_I2C_STOP_SM_CYCLES, PIO_I2C_CYCLES_PER_BIT)
#define PIO_I2C_STOP_CLOCKS \
	(PIO_I2C_STOP_TRANSFER_CLOCKS + PIO_I2C_IDLE_CONFIRM_CLOCKS)
#define PIO_I2C_EXEC_COUNT(instruction_count) \
	(((instruction_count) - 1U) << PIO_I2C_ICOUNT_LSB)

#define PIO_I2C_MIN_TIMING_OK(cycles, nanoseconds) \
	((uint64_t)(cycles) * PIO_I2C_NSEC_PER_SEC >= \
	 (uint64_t)(nanoseconds) * PIO_I2C_CYCLES_PER_BIT * PIO_I2C_MAX_REQUEST_HZ)
#define PIO_I2C_MAX_TIMING_OK(cycles, nanoseconds) \
	((uint64_t)(cycles) * PIO_I2C_NSEC_PER_SEC <= \
	 (uint64_t)(nanoseconds) * PIO_I2C_CYCLES_PER_BIT * PIO_I2C_MAX_REQUEST_HZ)

BUILD_ASSERT(PIO_I2C_DATA_TLOW_CYCLES == 18U);
BUILD_ASSERT(PIO_I2C_DATA_THIGH_CYCLES == 16U);
BUILD_ASSERT(PIO_I2C_CYCLES_PER_BIT == 34U);
BUILD_ASSERT(PIO_I2C_ACK_TLOW_CYCLES == 18U);
BUILD_ASSERT(PIO_I2C_ACK_THIGH_CYCLES == 16U);
BUILD_ASSERT(PIO_I2C_ESCAPE_LEVEL_CYCLES == 10U);
BUILD_ASSERT(PIO_I2C_MIN_TIMING_OK(PIO_I2C_DATA_TLOW_CYCLES,
				    PIO_I2C_FAST_TLOW_MIN_NS));
BUILD_ASSERT(PIO_I2C_MIN_TIMING_OK(PIO_I2C_DATA_THIGH_CYCLES,
				    PIO_I2C_FAST_THIGH_MIN_NS));
BUILD_ASSERT(PIO_I2C_MIN_TIMING_OK(PIO_I2C_DATA_SETUP_CYCLES,
				    PIO_I2C_FAST_TSU_DAT_MIN_NS));
BUILD_ASSERT(PIO_I2C_MAX_TIMING_OK(PIO_I2C_DATA_VALID_CYCLES,
				    PIO_I2C_FAST_TVD_DAT_MAX_NS));
BUILD_ASSERT(PIO_I2C_MIN_TIMING_OK(PIO_I2C_ACK_TLOW_CYCLES,
				    PIO_I2C_FAST_TLOW_MIN_NS));
BUILD_ASSERT(PIO_I2C_MIN_TIMING_OK(PIO_I2C_ACK_THIGH_CYCLES,
				    PIO_I2C_FAST_THIGH_MIN_NS));
BUILD_ASSERT(PIO_I2C_MAX_TIMING_OK(PIO_I2C_ACK_VALID_CYCLES,
				    PIO_I2C_FAST_TVD_ACK_MAX_NS));
BUILD_ASSERT(PIO_I2C_MIN_TIMING_OK(PIO_I2C_START_HOLD_CYCLES,
				    PIO_I2C_FAST_THD_STA_MIN_NS));
BUILD_ASSERT(PIO_I2C_MIN_TIMING_OK(PIO_I2C_REPSTART_SETUP_CYCLES,
				    PIO_I2C_FAST_TSU_STA_MIN_NS));
BUILD_ASSERT(PIO_I2C_MIN_TIMING_OK(PIO_I2C_STOP_SETUP_CYCLES,
				    PIO_I2C_FAST_TSU_STO_MIN_NS));
BUILD_ASSERT(PIO_I2C_MIN_TIMING_OK(PIO_I2C_RECOVERY_STOP_TLOW_CYCLES,
				    PIO_I2C_FAST_TLOW_MIN_NS));
BUILD_ASSERT(PIO_I2C_MIN_TIMING_OK(PIO_I2C_RECOVERY_STOP_SETUP_CYCLES,
				    PIO_I2C_FAST_TSU_STO_MIN_NS));
BUILD_ASSERT(PIO_I2C_IDLE_SAMPLE_MIN_USEC * 1000U >= PIO_I2C_FAST_TBUF_MIN_NS);
BUILD_ASSERT(PIO_I2C_RECOVERY_CYCLES_PER_PULSE >= PIO_I2C_CYCLES_PER_BIT);
BUILD_ASSERT(PIO_I2C_RECOVERY_TRANSFER_CLOCKS == 13U);
BUILD_ASSERT(PIO_I2C_RECOVERY_CLOCKS == 15U);
BUILD_ASSERT(PIO_I2C_STOP_CLOCKS == 3U);

/* pioasm 2.2.0 output for the pico-examples program with explicit side-low
 * holds after the data and ACK OUTs (wrap 14..19, entry 14).
 */
RPI_PICO_PIO_DEFINE_PROGRAM(i2c_pio, 14, 19,
	0x008e, /*  0: jmp    y--, 14                     */
	0xc030, /*  1: irq    wait 0 rel                  */
	PIO_I2C_INSTR_SET_X_7,         /*  2: set    x, 7                        */
	PIO_I2C_INSTR_DATA_OUT,        /*  3: out    pindirs, 1             [7]  */
	PIO_I2C_INSTR_DATA_LOW_HOLD,   /*  4: nop                    side 0 [1]  */
	PIO_I2C_INSTR_DATA_HIGH,       /*  5: nop                    side 1 [2]  */
	PIO_I2C_INSTR_DATA_WAIT,       /*  6: wait   1 gpio, 20             [4]  */
	PIO_I2C_INSTR_DATA_IN,         /*  7: in     pins, 1                [7]  */
	PIO_I2C_INSTR_DATA_FALL,       /*  8: jmp    x--, 3          side 0 [7]  */
	PIO_I2C_INSTR_DATA_OUT,        /*  9: out    pindirs, 1             [7]  */
	PIO_I2C_INSTR_ACK_LOW_HOLD,    /* 10: nop                    side 0 [2]  */
	PIO_I2C_INSTR_ACK_HIGH,        /* 11: nop                    side 1 [7]  */
	PIO_I2C_INSTR_ACK_WAIT,        /* 12: wait   1 gpio, 20             [7]  */
	PIO_I2C_INSTR_ACK_FALL,        /* 13: jmp    pin, 0          side 0 [3]  */
	PIO_I2C_INSTR_PARSER_OUT_X,    /* 14: out    x, 6                        */
	PIO_I2C_INSTR_PARSER_OUT_Y,    /* 15: out    y, 1                        */
	PIO_I2C_INSTR_PARSER_JMP_DATA, /* 16: jmp    !x, 2                       */
	PIO_I2C_INSTR_PARSER_OUT_NULL, /* 17: out    null, 32                    */
	PIO_I2C_INSTR_OUT_EXEC,        /* 18: out    exec, 16                    */
	PIO_I2C_INSTR_EXEC_LOOP,       /* 19: jmp    x--, 18                     */
);

static const uint16_t pio_i2c_recovery_instructions[] = {
	/* Each eight-cycle raw state also costs one OUT EXEC and one loop jump.
	 * Duplicating low and high therefore makes each recovery pulse 40 SM
	 * cycles, versus 34 for a normal data bit: about 340 kHz only when DT
	 * explicitly requests the 400 kHz capability ceiling. The STOP tail likewise
	 * duplicates SCL-low/SDA-low and
	 * SCL-high/SDA-low before releasing SDA. Recovery can only be slower than
	 * the configured bus, including its final clock-shaped transition.
	 */
	PIO_I2C_SC0_SD1, PIO_I2C_SC0_SD1, PIO_I2C_SC1_SD1, PIO_I2C_SC1_SD1,
	PIO_I2C_SC0_SD1, PIO_I2C_SC0_SD1, PIO_I2C_SC1_SD1, PIO_I2C_SC1_SD1,
	PIO_I2C_SC0_SD1, PIO_I2C_SC0_SD1, PIO_I2C_SC1_SD1, PIO_I2C_SC1_SD1,
	PIO_I2C_SC0_SD1, PIO_I2C_SC0_SD1, PIO_I2C_SC1_SD1, PIO_I2C_SC1_SD1,
	PIO_I2C_SC0_SD1, PIO_I2C_SC0_SD1, PIO_I2C_SC1_SD1, PIO_I2C_SC1_SD1,
	PIO_I2C_SC0_SD1, PIO_I2C_SC0_SD1, PIO_I2C_SC1_SD1, PIO_I2C_SC1_SD1,
	PIO_I2C_SC0_SD1, PIO_I2C_SC0_SD1, PIO_I2C_SC1_SD1, PIO_I2C_SC1_SD1,
	PIO_I2C_SC0_SD1, PIO_I2C_SC0_SD1, PIO_I2C_SC1_SD1, PIO_I2C_SC1_SD1,
	PIO_I2C_SC0_SD1, PIO_I2C_SC0_SD1, PIO_I2C_SC1_SD1, PIO_I2C_SC1_SD1,
	PIO_I2C_SC0_SD0, PIO_I2C_SC0_SD0,
	PIO_I2C_SC1_SD0, PIO_I2C_SC1_SD0, PIO_I2C_SC1_SD1,
};

BUILD_ASSERT(ARRAY_SIZE(pio_i2c_recovery_instructions) ==
	     PIO_I2C_RECOVERY_INSTRUCTION_COUNT);

struct pio_i2c_config {
	const struct device *piodev;
	const struct pinctrl_dev_config *pcfg;
	uint32_t clock_frequency;
	uint32_t sda_pin;
	uint32_t scl_pin;
};

struct pio_i2c_data {
	size_t sm;
	int64_t deadline_ms;
	int64_t recovery_not_before_ms;
	uint32_t effective_frequency;
	bool fail_closed;
};

enum pio_i2c_line_state {
	PIO_I2C_LINES_UNSTABLE,
	PIO_I2C_LINES_IDLE,
	PIO_I2C_LINES_SDA_LOW,
	PIO_I2C_LINES_SCL_LOW,
};

static K_MUTEX_DEFINE(pio_i2c_bus_lock);

static uint32_t pio_i2c_timeout_ms(uint32_t clock_frequency, uint64_t bus_clocks,
				   uint32_t minimum_ms)
{
	const uint64_t scale = PIO_I2C_TIMEOUT_MARGIN * PIO_I2C_MSEC_PER_SEC;
	uint64_t numerator;
	uint64_t timeout_ms;

	if (clock_frequency == 0U) {
		return PIO_I2C_TIMEOUT_MAX_MSEC;
	}

	/* Cap before multiplying so even a saturated clock count cannot wrap. */
	if (bus_clocks > ((uint64_t)PIO_I2C_TIMEOUT_MAX_MSEC *
			  clock_frequency) /
			 scale) {
		return PIO_I2C_TIMEOUT_MAX_MSEC;
	}

	numerator = bus_clocks * scale;
	timeout_ms = numerator / clock_frequency;
	if ((numerator % clock_frequency) != 0U) {
		timeout_ms++;
	}
	if (timeout_ms < minimum_ms) {
		timeout_ms = minimum_ms;
	}
	if (timeout_ms > PIO_I2C_TIMEOUT_MAX_MSEC) {
		timeout_ms = PIO_I2C_TIMEOUT_MAX_MSEC;
	}

	return (uint32_t)timeout_ms;
}

static uint64_t pio_i2c_transaction_clocks(const struct i2c_msg *msgs,
					   uint8_t num_msgs)
{
	uint64_t total = 0U;

	for (uint8_t i = 0U; i < num_msgs; i++) {
		uint64_t bytes = (uint64_t)msgs[i].len + 1U;
		uint64_t msg_clocks;

		if (bytes > (UINT64_MAX - PIO_I2C_MSG_OVERHEAD_CLOCKS) /
				    PIO_I2C_CLOCKS_PER_BYTE) {
			return UINT64_MAX;
		}
		msg_clocks = bytes * PIO_I2C_CLOCKS_PER_BYTE +
			     PIO_I2C_MSG_OVERHEAD_CLOCKS;
		if (total > UINT64_MAX - msg_clocks) {
			return UINT64_MAX;
		}
		total += msg_clocks;
	}

	return total;
}

static void pio_i2c_set_deadline(struct pio_i2c_data *data,
				 uint64_t bus_clocks, uint32_t minimum_ms)
{
	data->deadline_ms = k_uptime_get() +
		pio_i2c_timeout_ms(data->effective_frequency, bus_clocks, minimum_ms);
}

static inline bool pio_i2c_expired(const struct pio_i2c_data *data)
{
	return k_uptime_get() >= data->deadline_ms;
}

static inline bool pio_i2c_bus_is_idle(const struct pio_i2c_config *config)
{
	return gpio_get(config->scl_pin) && gpio_get(config->sda_pin);
}

static uint32_t pio_i2c_sample_interval_us(uint32_t clock_frequency)
{
	uint64_t interval;
	const uint32_t stable_interval_min =
		(PIO_I2C_STABLE_MIN_USEC + PIO_I2C_STABLE_SAMPLES - 2U) /
		(PIO_I2C_STABLE_SAMPLES - 1U);

	if (clock_frequency == 0U) {
		return PIO_I2C_SAMPLE_MAX_USEC;
	}
	interval = ((uint64_t)PIO_I2C_USEC_PER_SEC + clock_frequency - 1U) /
		   clock_frequency;
	if (interval < stable_interval_min) {
		interval = stable_interval_min;
	}
	if (interval > PIO_I2C_SAMPLE_MAX_USEC) {
		interval = PIO_I2C_SAMPLE_MAX_USEC;
	}

	return (uint32_t)interval;
}

static uint32_t pio_i2c_idle_sample_interval_us(uint32_t clock_frequency)
{
	uint64_t interval;

	if (clock_frequency == 0U) {
		return PIO_I2C_SAMPLE_MAX_USEC;
	}
	interval = ((uint64_t)PIO_I2C_USEC_PER_SEC + clock_frequency - 1U) /
		   clock_frequency;
	if (interval < PIO_I2C_IDLE_SAMPLE_MIN_USEC) {
		interval = PIO_I2C_IDLE_SAMPLE_MIN_USEC;
	}
	if (interval > PIO_I2C_SAMPLE_MAX_USEC) {
		interval = PIO_I2C_SAMPLE_MAX_USEC;
	}

	return (uint32_t)interval;
}

static enum pio_i2c_line_state
pio_i2c_sample_stable_lines(const struct pio_i2c_config *config,
			    uint32_t clock_frequency)
{
	uint32_t interval_us = pio_i2c_sample_interval_us(clock_frequency);
	uint8_t previous = 0xffU;
	uint8_t consecutive = 0U;

	for (uint8_t i = 0U; i < PIO_I2C_STABLE_SAMPLES * 2U; i++) {
		uint8_t sample = (gpio_get(config->scl_pin) ? BIT(1) : 0U) |
				 (gpio_get(config->sda_pin) ? BIT(0) : 0U);

		if (sample == previous) {
			consecutive++;
		} else {
			previous = sample;
			consecutive = 1U;
		}
		if (consecutive >= PIO_I2C_STABLE_SAMPLES) {
			switch (sample) {
			case BIT(1) | BIT(0):
				return PIO_I2C_LINES_IDLE;
			case BIT(1):
				return PIO_I2C_LINES_SDA_LOW;
			default:
				return PIO_I2C_LINES_SCL_LOW;
			}
		}
		k_usleep(interval_us);
	}

	return PIO_I2C_LINES_UNSTABLE;
}

static bool pio_i2c_wait_bus_idle_until(const struct pio_i2c_config *config,
					uint32_t clock_frequency, int64_t deadline)
{
	/* Healthy transfers only need a short, tBUF-safe confirmation. Fault
	 * classification and every recovery decision use the longer 5 ms stable
	 * sampler above, so ordinary traffic does not inherit recovery debounce.
	 */
	uint32_t interval_us = pio_i2c_idle_sample_interval_us(clock_frequency);
	uint8_t consecutive = 0U;

	do {
		if (pio_i2c_bus_is_idle(config)) {
			consecutive++;
			if (consecutive >= PIO_I2C_IDLE_SAMPLES) {
				return true;
			}
		} else {
			consecutive = 0U;
		}
		/* This is the healthy-traffic tBUF confirmation: 5 us even at the
		 * supported 400 kHz ceiling, above Fast-mode's 1.3 us minimum.
		 * k_usleep() rounds short waits to kernel ticks and would dominate the
		 * eight INA226 register transactions in every telemetry sample.
		 */
		k_busy_wait(interval_us);
	} while (k_uptime_get() <= deadline);

	return false;
}

static bool pio_i2c_wait_bus_idle(const struct pio_i2c_config *config,
				  uint32_t clock_frequency)
{
	int64_t deadline = k_uptime_get() +
		pio_i2c_timeout_ms(clock_frequency, PIO_I2C_BUS_IDLE_CLOCKS,
				       PIO_I2C_TIMEOUT_MIN_MSEC);

	return pio_i2c_wait_bus_idle_until(config, clock_frequency, deadline);
}

static inline PIO pio_i2c_pio(const struct device *dev)
{
	const struct pio_i2c_config *config = dev->config;

	return pio_rpi_pico_get_pio(config->piodev);
}

static bool pio_i2c_check_error(PIO pio, size_t sm)
{
	return pio_interrupt_get(pio, sm);
}

static int pio_i2c_status(const struct pio_i2c_data *data, PIO pio, size_t sm)
{
	if (pio_i2c_expired(data)) {
		return -ETIMEDOUT;
	}
	if (pio_i2c_check_error(pio, sm)) {
		return -EIO;
	}

	return 0;
}

static inline int pio_i2c_put16(const struct pio_i2c_data *data, PIO pio,
				size_t sm, uint16_t word)
{
	int ret;

	do {
		ret = pio_i2c_status(data, pio, sm);
		if (ret < 0) {
			return ret;
		}
	} while (pio_sm_is_tx_fifo_full(pio, sm));

	*(io_rw_16 *)&pio->txf[sm] = word;
	return 0;
}

static void pio_i2c_resume_after_error(PIO pio, size_t sm, uint32_t both_pins)
{
	uint32_t wrap_bottom;

	pio_sm_drain_tx_fifo(pio, sm);
	pio_sm_set_enabled(pio, sm, false);

	/* Release both lines before restarting so a failed transfer cannot
	 * leave SDA/SCL driven low. The SDK pin helpers require a disabled SM.
	 */
	pio_sm_set_pins_with_mask(pio, sm, 0U, both_pins);
	pio_sm_set_pindirs_with_mask(pio, sm, both_pins, both_pins);
	wrap_bottom = (pio->sm[sm].execctrl & PIO_SM0_EXECCTRL_WRAP_BOTTOM_BITS) >>
		      PIO_SM0_EXECCTRL_WRAP_BOTTOM_LSB;
	pio_sm_exec(pio, sm, pio_encode_jmp(wrap_bottom));
	pio_interrupt_clear(pio, sm);
	pio_sm_set_enabled(pio, sm, true);
}

static inline int pio_i2c_put_or_err(const struct pio_i2c_data *data, PIO pio,
				    size_t sm, uint16_t word)
{
	return pio_i2c_put16(data, pio, sm, word);
}

static void pio_i2c_rx_enable(PIO pio, size_t sm, bool enable)
{
	if (enable) {
		hw_set_bits(&pio->sm[sm].shiftctrl, PIO_SM0_SHIFTCTRL_AUTOPUSH_BITS);
	} else {
		hw_clear_bits(&pio->sm[sm].shiftctrl, PIO_SM0_SHIFTCTRL_AUTOPUSH_BITS);
	}
}

static uint8_t pio_i2c_get(PIO pio, size_t sm)
{
	return (uint8_t)pio_sm_get(pio, sm);
}

static int pio_i2c_queue_escape(const struct pio_i2c_data *data, PIO pio,
				size_t sm, const uint16_t *instructions,
				size_t instruction_count)
{
	int ret;

	if (instruction_count < 2U || instruction_count > 64U) {
		return -EINVAL;
	}

	ret = pio_i2c_put_or_err(data, pio, sm,
				  (uint16_t)PIO_I2C_EXEC_COUNT(instruction_count));
	for (size_t i = 0U; ret == 0 && i < instruction_count; i++) {
		ret = pio_i2c_put_or_err(data, pio, sm, instructions[i]);
	}

	/* A partial escape is an error; callers must reset or fail-close the SM. */
	return ret;
}

static int pio_i2c_start(const struct pio_i2c_data *data, PIO pio, size_t sm)
{
	const uint16_t instructions[] = {
		PIO_I2C_SC1_SD0,
		PIO_I2C_SC0_SD0,
		pio_encode_mov(pio_isr, pio_null),
	};

	/* The bus was verified idle: generate START, hold SCL low, then clear the
	 * ISR before the address byte. REPSTART additionally releases both lines.
	 */
	return pio_i2c_queue_escape(data, pio, sm, instructions,
				     ARRAY_SIZE(instructions));
}

static int pio_i2c_stop(const struct pio_i2c_data *data, PIO pio, size_t sm)
{
	const uint16_t instructions[] = {
		PIO_I2C_SC0_SD0,
		PIO_I2C_SC1_SD0,
		PIO_I2C_SC1_SD1,
	};

	/* Clear a prior empty-FIFO event before queueing STOP. Clearing it in
	 * wait_idle() could erase STOP's completion if the PIO wins the race.
	 */
	pio->fdebug = 1U << (PIO_FDEBUG_TXSTALL_LSB + sm);
	return pio_i2c_queue_escape(data, pio, sm, instructions,
				     ARRAY_SIZE(instructions));
}

static int pio_i2c_repstart(const struct pio_i2c_data *data, PIO pio, size_t sm)
{
	const uint16_t instructions[] = {
		PIO_I2C_SC0_SD1,
		PIO_I2C_SC1_SD1,
		PIO_I2C_SC1_SD0,
		PIO_I2C_SC0_SD0,
		pio_encode_mov(pio_isr, pio_null),
	};

	return pio_i2c_queue_escape(data, pio, sm, instructions,
				     ARRAY_SIZE(instructions));
}

static int pio_i2c_wait_idle(const struct pio_i2c_data *data, PIO pio, size_t sm)
{
	uint32_t wrap_bottom =
		(pio->sm[sm].execctrl & PIO_SM0_EXECCTRL_WRAP_BOTTOM_BITS) >>
		PIO_SM0_EXECCTRL_WRAP_BOTTOM_LSB;
	int ret;

	/* TXSTALL is sticky and may have been raised by a temporary FIFO
	 * underrun while an escape record was still being queued. Completion
	 * requires the FIFO to be empty and the SM to have returned to the parser
	 * entry, stalled on the next record's OUT X rather than inside OUT EXEC.
	 */
	while (!(pio->fdebug & (1U << (PIO_FDEBUG_TXSTALL_LSB + sm))) ||
	       !pio_sm_is_tx_fifo_empty(pio, sm) ||
	       pio_sm_get_pc(pio, sm) != wrap_bottom) {
		ret = pio_i2c_status(data, pio, sm);
		if (ret < 0) {
			return ret;
		}
	}

	return pio_i2c_status(data, pio, sm);
}

static void pio_i2c_reset_and_release(const struct device *dev)
{
	const struct pio_i2c_config *config = dev->config;
	struct pio_i2c_data *data = dev->data;
	PIO pio = pio_i2c_pio(dev);
	size_t sm = data->sm;
	uint32_t both_pins = (1U << config->sda_pin) | (1U << config->scl_pin);
	uint32_t wrap_bottom;

	/* GPIO_OVERRIDE_LOW disables the physical output enable. Keep it forced
	 * off while discarding any partial record and restoring the PIO entry.
	 */
	gpio_set_oeover(config->sda_pin, GPIO_OVERRIDE_LOW);
	gpio_set_oeover(config->scl_pin, GPIO_OVERRIDE_LOW);
	pio_sm_set_enabled(pio, sm, false);
	pio_sm_clear_fifos(pio, sm);
	pio_sm_restart(pio, sm);
	pio_interrupt_clear(pio, sm);
	wrap_bottom = (pio->sm[sm].execctrl & PIO_SM0_EXECCTRL_WRAP_BOTTOM_BITS) >>
		      PIO_SM0_EXECCTRL_WRAP_BOTTOM_LSB;
	pio_sm_exec(pio, sm, pio_encode_jmp(wrap_bottom));
	pio_sm_set_pins_with_mask(pio, sm, 0U, both_pins);

	/* This driver implements open drain by inverting the peripheral OE. A
	 * PIO pindir value of one therefore means released once INVERT is restored.
	 */
	pio_sm_set_pindirs_with_mask(pio, sm, both_pins, both_pins);
}

static void pio_i2c_fail_close(const struct device *dev)
{
	struct pio_i2c_data *data = dev->data;

	pio_i2c_reset_and_release(dev);
	data->fail_closed = true;
}

static void pio_i2c_rearm(const struct device *dev)
{
	const struct pio_i2c_config *config = dev->config;
	struct pio_i2c_data *data = dev->data;
	PIO pio = pio_i2c_pio(dev);

	pio_i2c_reset_and_release(dev);
	gpio_set_oeover(config->sda_pin, GPIO_OVERRIDE_INVERT);
	gpio_set_oeover(config->scl_pin, GPIO_OVERRIDE_INVERT);
	pio_sm_set_enabled(pio, data->sm, true);
	data->fail_closed = false;
}

static int pio_i2c_cleanup_after_irq(const struct device *dev)
{
	const struct pio_i2c_config *config = dev->config;
	struct pio_i2c_data *data = dev->data;
	PIO pio = pio_i2c_pio(dev);
	uint32_t both_pins = (1U << config->sda_pin) | (1U << config->scl_pin);
	int ret;

	pio_i2c_resume_after_error(pio, data->sm, both_pins);
	pio_i2c_set_deadline(data, PIO_I2C_STOP_CLOCKS,
			     PIO_I2C_TIMEOUT_MIN_MSEC);
	ret = pio_i2c_stop(data, pio, data->sm);
	if (ret == 0) {
		ret = pio_i2c_wait_idle(data, pio, data->sm);
	}
	if (ret == 0 && !pio_i2c_wait_bus_idle_until(config, data->effective_frequency,
						    data->deadline_ms)) {
		ret = -EBUSY;
	}
	if (ret < 0) {
		pio_i2c_fail_close(dev);
	}

	return ret;
}

static int pio_i2c_recover_bus(const struct device *dev);
static int pio_i2c_recover_locked(const struct device *dev);

static int pio_i2c_transfer_once(const struct device *dev, struct i2c_msg *msgs,
				 uint8_t num_msgs, uint16_t addr)
{
	const struct pio_i2c_config *config = dev->config;
	struct pio_i2c_data *data = dev->data;
	PIO pio = pio_i2c_pio(dev);
	size_t sm = data->sm;
	uint64_t transaction_clocks;
	uint32_t lock_timeout_ms;
	int err = 0;

	if (msgs == NULL || num_msgs == 0U) {
		return -EINVAL;
	}
	if ((addr & ~0x7FU) != 0U) {
		return -EINVAL;
	}
	for (uint8_t i = 0U; i < num_msgs; i++) {
		if ((msgs[i].flags & I2C_MSG_ADDR_10_BITS) != 0U) {
			return -EINVAL;
		}
		/* This driver joins fragments with REPSTART. Supporting an in-list
		 * STOP would also require an explicit tBUF delay before the next START.
		 */
		if (i + 1U < num_msgs && (msgs[i].flags & I2C_MSG_STOP) != 0U) {
			return -ENOTSUP;
		}
	}

	transaction_clocks = pio_i2c_transaction_clocks(msgs, num_msgs);
	lock_timeout_ms = pio_i2c_timeout_ms(data->effective_frequency, transaction_clocks,
					  PIO_I2C_LOCK_MIN_MSEC);
	if (k_mutex_lock(&pio_i2c_bus_lock, K_MSEC(lock_timeout_ms)) != 0) {
		return -EBUSY;
	}

	if (data->fail_closed) {
		/* Automatic retry may only rearm a bus that has already become idle.
		 * It must not emit the recovery clock train.
		 */
		if (pio_i2c_sample_stable_lines(config, data->effective_frequency) !=
		    PIO_I2C_LINES_IDLE) {
			err = -EBUSY;
			goto unlock;
		}
		pio_i2c_rearm(dev);
	}
	if (!pio_i2c_wait_bus_idle(config, data->effective_frequency)) {
		pio_i2c_fail_close(dev);
		err = -EBUSY;
		goto unlock;
	}

	pio_i2c_set_deadline(data, transaction_clocks,
			     PIO_I2C_TIMEOUT_MIN_MSEC);

	for (uint8_t i = 0; i < num_msgs; i++) {
		struct i2c_msg *msg = &msgs[i];
		bool reading = (msg->flags & I2C_MSG_READ) != 0U;

		if (i == 0U) {
			err = pio_i2c_start(data, pio, sm);
		} else {
			err = pio_i2c_repstart(data, pio, sm);
		}
		if (err < 0) {
			goto out;
		}

		pio_i2c_rx_enable(pio, sm, reading);
		while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
			(void)pio_i2c_get(pio, sm);
		}

		err = pio_i2c_put16(data, pio, sm,
				     (uint16_t)((addr << 2) | (reading ? 3U : 1U)));
		if (err < 0) {
			goto out;
		}

		if (!reading) {
			for (size_t n = 0U; n < msg->len; n++) {
				/* FINAL suppresses the target's NAK in this PIO program and is
				 * therefore only valid for the controller-generated final NAK
				 * of a read. Every write byte, including the last, must be ACKed.
				 */
				uint16_t word =
					(uint16_t)((msg->buf[n] << PIO_I2C_DATA_LSB) | 1U);

				err = pio_i2c_put16(data, pio, sm, word);
				if (err < 0) {
					goto out;
				}
			}
		} else {
			size_t tx_remain = msg->len;
			size_t rx_remain = msg->len;
			bool first = true;

			while (tx_remain || rx_remain) {
				err = pio_i2c_status(data, pio, sm);
				if (err < 0) {
					goto out;
				}
				if (tx_remain && !pio_sm_is_tx_fifo_full(pio, sm)) {
					uint16_t word;

					tx_remain--;
					word = (uint16_t)((0xffU << PIO_I2C_DATA_LSB) |
							  ((tx_remain == 0U) ? (1U << PIO_I2C_FINAL_LSB) : 0U) |
							  ((tx_remain == 0U) ? (1U << PIO_I2C_NAK_LSB) : 0U));
					err = pio_i2c_put16(data, pio, sm, word);
					if (err < 0) {
						goto out;
					}
				}
				if (!pio_sm_is_rx_fifo_empty(pio, sm)) {
					uint8_t value = pio_i2c_get(pio, sm);

					if (first) {
						first = false;
					} else {
						msg->buf[msg->len - rx_remain] = value;
						rx_remain--;
					}
				}
			}
		}

		if ((msg->flags & I2C_MSG_STOP) != 0U) {
			err = pio_i2c_stop(data, pio, sm);
			if (err == 0) {
				err = pio_i2c_wait_idle(data, pio, sm);
			}
			if (err < 0) {
				goto out;
			}
		}
	}

	if (!(msgs[num_msgs - 1U].flags & I2C_MSG_STOP)) {
		err = pio_i2c_stop(data, pio, sm);
		if (err == 0) {
			err = pio_i2c_wait_idle(data, pio, sm);
		}
	}

	if (err == 0) {
		err = pio_i2c_status(data, pio, sm);
	}

out:
	if (err != 0) {
		/* Only a PIO IRQ denotes the clean NAK wait state. A timeout or any
		 * other error may have stopped in the middle of an escape record, so
		 * feeding STOP into that stream would reinterpret instructions as data.
		 */
		if (err == -EIO && pio_i2c_check_error(pio, sm)) {
			int cleanup_err = pio_i2c_cleanup_after_irq(dev);

			if (cleanup_err < 0) {
				err = cleanup_err;
			}
		} else {
			pio_i2c_fail_close(dev);
		}
	} else if (!pio_i2c_wait_bus_idle_until(config, data->effective_frequency,
					       data->deadline_ms)) {
		/* A device retained the bus after STOP. Fail closed until recovery. */
		pio_i2c_fail_close(dev);
		err = -EBUSY;
	}

unlock:
	k_mutex_unlock(&pio_i2c_bus_lock);
	return err;
}

static int pio_i2c_transfer(const struct device *dev, struct i2c_msg *msgs,
			    uint8_t num_msgs, uint16_t addr)
{
	return pio_i2c_transfer_once(dev, msgs, num_msgs, addr);
}

static int pio_i2c_calculate_clkdiv(uint32_t sys_hz, uint32_t bus_hz,
				    uint32_t *div_int, uint8_t *div_frac,
				    uint32_t *effective_hz)
{
	uint64_t denominator;
	uint64_t scaled_div;
	const uint64_t max_scaled_div =
		(uint64_t)PIO_I2C_CLKDIV_MAX * PIO_I2C_CLKDIV_SCALE;

	if (sys_hz == 0U || bus_hz == 0U) {
		return -EINVAL;
	}

	denominator = (uint64_t)PIO_I2C_CYCLES_PER_BIT * bus_hz;
	if (denominator > sys_hz ||
	    (uint64_t)sys_hz > denominator * PIO_I2C_CLKDIV_MAX) {
		return -EINVAL;
	}

	/* Round the divider up so the effective bus clock never exceeds the
	 * requested timing target.
	 */
	scaled_div = ((uint64_t)sys_hz * PIO_I2C_CLKDIV_SCALE + denominator - 1U) /
		     denominator;
	if (scaled_div == max_scaled_div) {
		/* An integer field of zero encodes 65536 in the PIO divider. */
		*div_int = 0U;
		*div_frac = 0U;
	} else {
		*div_int = (uint32_t)(scaled_div / PIO_I2C_CLKDIV_SCALE);
		*div_frac = (uint8_t)(scaled_div % PIO_I2C_CLKDIV_SCALE);
	}
	*effective_hz = (uint32_t)(((uint64_t)sys_hz * PIO_I2C_CLKDIV_SCALE) /
				  (PIO_I2C_CYCLES_PER_BIT * scaled_div));
	if (*effective_hz == 0U) {
		return -EINVAL;
	}

	return 0;
}

static int pio_i2c_configure(const struct device *dev, uint32_t dev_config)
{
	const struct pio_i2c_config *config = dev->config;
	uint32_t speed = I2C_SPEED_GET(dev_config);
	uint32_t hz;

	if ((dev_config & I2C_MODE_CONTROLLER) == 0U ||
	    (dev_config & I2C_ADDR_10_BITS) != 0U) {
		return -EINVAL;
	}

	/*
	 * The PIO divider is fixed once at init from devicetree. The Zephyr speed
	 * field has no arbitrary-Hz representation, so I2C_SPEED_DT validates that
	 * fixed rate without pretending to change it at run time.
	 */
	switch (speed) {
	case I2C_SPEED_DT:
		return 0;
	case I2C_SPEED_STANDARD:
		hz = 100000U;
		break;
	case I2C_SPEED_FAST:
		hz = 400000U;
		break;
	default:
		return -EINVAL;
	}

	return hz == config->clock_frequency ? 0 : -EINVAL;
}

static int pio_i2c_init(const struct device *dev)
{
	const struct pio_i2c_config *config = dev->config;
	struct pio_i2c_data *data = dev->data;
	pio_sm_config sm_config;
	uint32_t offset;
	uint32_t div_int;
	uint32_t sys_hz;
	uint32_t init_settle_ms;
	uint8_t div_frac;
	size_t sm;
	int64_t init_deadline;
	int ret;
	PIO pio;
	uint32_t both_pins;

	if (!device_is_ready(config->piodev)) {
		return -ENODEV;
	}
	if (config->clock_frequency > PIO_I2C_MAX_REQUEST_HZ) {
		LOG_ERR("PIO I2C frequency %u Hz exceeds the supported 400 kHz maximum",
			config->clock_frequency);
		return -EINVAL;
	}
	sys_hz = clock_get_hz(clk_sys);
	ret = pio_i2c_calculate_clkdiv(sys_hz, config->clock_frequency,
					&div_int, &div_frac,
					&data->effective_frequency);
	if (ret < 0) {
		LOG_ERR("PIO I2C frequency %u Hz is outside the clk_sys=%u Hz divider range",
			config->clock_frequency, sys_hz);
		return ret;
	}
	if (data->effective_frequency != config->clock_frequency) {
		LOG_INF("PIO I2C requested %u Hz, effective %u Hz after divider rounding",
			config->clock_frequency, data->effective_frequency);
	}

	pio = pio_rpi_pico_get_pio(config->piodev);

	ret = pio_rpi_pico_allocate_sm(config->piodev, &sm);
	if (ret < 0) {
		return ret;
	}
	data->sm = sm;
	data->fail_closed = true;
	data->recovery_not_before_ms = 0;

	if (!pio_can_add_program(pio, RPI_PICO_PIO_GET_PROGRAM(i2c_pio))) {
		pio_sm_unclaim(pio, sm);
		return -EBUSY;
	}
	offset = pio_add_program(pio, RPI_PICO_PIO_GET_PROGRAM(i2c_pio));

	sm_config = pio_get_default_sm_config();
	sm_config_set_out_pins(&sm_config, config->sda_pin, 1);
	sm_config_set_set_pins(&sm_config, config->sda_pin, 1);
	sm_config_set_in_pins(&sm_config, config->sda_pin);
	sm_config_set_sideset_pins(&sm_config, config->scl_pin);
	sm_config_set_jmp_pin(&sm_config, config->sda_pin);
	sm_config_set_out_shift(&sm_config, false, true, 16);
	sm_config_set_in_shift(&sm_config, false, true, 8);
	sm_config_set_sideset(&sm_config, 2, true, true);
	sm_config_set_clkdiv_int_frac8(&sm_config, div_int, div_frac);
	sm_config_set_wrap(&sm_config, offset + RPI_PICO_PIO_GET_WRAP_TARGET(i2c_pio),
			   offset + RPI_PICO_PIO_GET_WRAP(i2c_pio));

	both_pins = (1U << config->sda_pin) | (1U << config->scl_pin);

	/*
	 * Force both pad output enables off before changing pinmux. The Pico SDK
	 * example briefly relies on the prepared PIO output state while OE is
	 * still in its normal mode. That is acceptable for a conventional I2C
	 * bus, but x1.2 also connects these pins to CH224A configuration inputs;
	 * never allow a push-pull transition on this shared bus.
	 */
	gpio_set_oeover(config->sda_pin, GPIO_OVERRIDE_LOW);
	gpio_set_oeover(config->scl_pin, GPIO_OVERRIDE_LOW);

	/* RP2350 pads reset with pull-down enabled. Disable both pulls while the
	 * pads are still isolated and physical OE is forced off. In the pinned
	 * Pico SDK, gpio_disable_pulls() only calls gpio_set_pulls(), whose masked
	 * PADS_BANK0 write touches PUE/PDE; it does not call gpio_init() or
	 * gpio_set_function(), clear ISO, select a mux, or change OE/overrides.
	 */
	gpio_disable_pulls(config->sda_pin);
	gpio_disable_pulls(config->scl_pin);

	ret = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	/* RP2 pinctrl calls gpio_init()/gpio_set_function(), which removes pad
	 * isolation and resets overrides. Reassert physical OE disable before
	 * checking the result so even a future partial pinctrl failure releases
	 * both lines before resources are unwound.
	 */
	gpio_set_oeover(config->sda_pin, GPIO_OVERRIDE_LOW);
	gpio_set_oeover(config->scl_pin, GPIO_OVERRIDE_LOW);
	if (ret < 0) {
		pio_remove_program_and_unclaim_sm(RPI_PICO_PIO_GET_PROGRAM(i2c_pio),
						  pio, sm, offset);
		return ret;
	}

	/* Prepare open-drain low values while OE remains forcibly disabled. */
	pio_sm_set_pins_with_mask(pio, sm, 0U, both_pins);
	pio_sm_set_pindirs_with_mask(pio, sm, both_pins, both_pins);
	gpio_set_drive_strength(config->sda_pin, GPIO_DRIVE_STRENGTH_4MA);
	gpio_set_drive_strength(config->scl_pin, GPIO_DRIVE_STRENGTH_4MA);

	pio_set_irq0_source_enabled(pio, (enum pio_interrupt_source)(pis_interrupt0 + sm), false);
	pio_set_irq1_source_enabled(pio, (enum pio_interrupt_source)(pis_interrupt0 + sm), false);
	pio_interrupt_clear(pio, sm);

	pio_sm_init(pio, sm,
		    offset + RPI_PICO_PIO_GET_WRAP_TARGET(i2c_pio), &sm_config);
	pio_i2c_fail_close(dev);

	/* Keep OE forcibly disabled while the cross-power-domain bus settles. The
	 * overall observation window is frequency-derived and capped at five
	 * seconds. No clock is emitted for low or unstable SCL.
	 */
	init_deadline = k_uptime_get() +
		pio_i2c_timeout_ms(data->effective_frequency, PIO_I2C_INIT_TOTAL_CLOCKS,
				       PIO_I2C_INIT_MIN_MSEC);
	init_settle_ms = pio_i2c_timeout_ms(data->effective_frequency,
					  PIO_I2C_INIT_SETTLE_CLOCKS,
					  PIO_I2C_TIMEOUT_MIN_MSEC);
	if (k_uptime_get() + init_settle_ms < init_deadline) {
		k_msleep(init_settle_ms);
	}

	do {
		enum pio_i2c_line_state state =
			pio_i2c_sample_stable_lines(config, data->effective_frequency);

		if (state == PIO_I2C_LINES_IDLE) {
			pio_i2c_rearm(dev);
			LOG_INF("PIO I2C bus idle after passive settle; controller active");
			return 0;
		}
		if (state == PIO_I2C_LINES_SDA_LOW) {
			ret = pio_i2c_recover_locked(dev);
			if (ret == 0) {
				LOG_INF("PIO I2C startup bus-clear succeeded; controller active");
				return 0;
			}
			LOG_WRN("PIO I2C startup bus-clear failed; controller degraded and paused (%d)",
				ret);
			return 0;
		}
	} while (k_uptime_get() <= init_deadline);

	/* A bus-level condition must not make the controller permanently not
	 * ready. Leave it safely paused so a later transfer can rearm after both
	 * lines become stably high, or an explicit recovery can be requested.
	 */
	pio_i2c_fail_close(dev);
	LOG_WRN("PIO I2C bus stayed low or unstable; controller degraded and paused "
		"(SCL=%d SDA=%d)",
		gpio_get(config->scl_pin), gpio_get(config->sda_pin));

	return 0;
}

static int pio_i2c_recover_locked(const struct device *dev)
{
	const struct pio_i2c_config *config = dev->config;
	struct pio_i2c_data *data = dev->data;
	PIO pio = pio_i2c_pio(dev);
	size_t sm = data->sm;
	enum pio_i2c_line_state state;
	int64_t now;
	int ret;

	/* First force both physical output enables off and restore a known,
	 * released state. Sampling happens only after that passive transition.
	 */
	pio_i2c_fail_close(dev);
	state = pio_i2c_sample_stable_lines(config, data->effective_frequency);
	if (state == PIO_I2C_LINES_IDLE) {
		pio_i2c_rearm(dev);
		return 0;
	}
	if (state != PIO_I2C_LINES_SDA_LOW) {
		return -EBUSY;
	}

	now = k_uptime_get();
	if (now < data->recovery_not_before_ms) {
		return -EAGAIN;
	}
	data->recovery_not_before_ms = now +
		pio_i2c_timeout_ms(data->effective_frequency,
				       PIO_I2C_RECOVERY_COOLDOWN_CLOCKS,
				       PIO_I2C_RECOVERY_COOLDOWN_MIN_MSEC);
	pio_i2c_set_deadline(data, PIO_I2C_RECOVERY_CLOCKS,
			     PIO_I2C_TIMEOUT_MIN_MSEC);
	gpio_set_oeover(config->sda_pin, GPIO_OVERRIDE_INVERT);
	gpio_set_oeover(config->scl_pin, GPIO_OVERRIDE_INVERT);
	pio_sm_set_enabled(pio, sm, true);

	/*
	 * Execute 18 SCL edges (nine open-drain pulses) followed by STOP. ICOUNT
	 * is the number of following instructions minus one. Raw set_scl_sda
	 * instructions must never be queued without this escape word.
	 */
	pio->fdebug = 1U << (PIO_FDEBUG_TXSTALL_LSB + sm);
	ret = pio_i2c_queue_escape(data, pio, sm, pio_i2c_recovery_instructions,
				    ARRAY_SIZE(pio_i2c_recovery_instructions));
	if (ret == 0) {
		ret = pio_i2c_wait_idle(data, pio, sm);
	}
	if (ret == 0 && !pio_i2c_wait_bus_idle_until(config, data->effective_frequency,
						    data->deadline_ms)) {
		ret = -EBUSY;
	}
	if (ret == 0) {
		pio_i2c_rearm(dev);
	} else {
		pio_i2c_fail_close(dev);
	}

	return ret;
}

static int pio_i2c_recover_bus(const struct device *dev)
{
	const struct pio_i2c_data *data = dev->data;
	uint32_t lock_timeout_ms =
		pio_i2c_timeout_ms(data->effective_frequency, PIO_I2C_RECOVERY_CLOCKS,
				       PIO_I2C_LOCK_MIN_MSEC);
	int ret;

	if (k_mutex_lock(&pio_i2c_bus_lock, K_MSEC(lock_timeout_ms)) != 0) {
		return -EBUSY;
	}

	ret = pio_i2c_recover_locked(dev);
	if (ret == 0) {
		LOG_INF("PIO I2C recovery completed; controller active");
	} else {
		LOG_WRN("PIO I2C recovery failed; controller remains paused (%d)", ret);
	}
	k_mutex_unlock(&pio_i2c_bus_lock);
	return ret;
}

static DEVICE_API(i2c, pio_i2c_driver_api) = {
	.configure = pio_i2c_configure,
	.transfer = pio_i2c_transfer,
	.recover_bus = pio_i2c_recover_bus,
};

#define PIO_I2C_INIT(idx)                                                                     \
	PINCTRL_DT_INST_DEFINE(idx);                                                          \
	static const struct pio_i2c_config pio_i2c_config_##idx = {                           \
		.piodev = DEVICE_DT_GET(DT_INST_PARENT(idx)),                                 \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(idx),                                  \
		.clock_frequency = DT_INST_PROP(idx, clock_frequency),                        \
		.sda_pin = DT_INST_RPI_PICO_PIO_PIN_BY_NAME(idx, default, 0, sda_pins, 0),    \
		.scl_pin = DT_INST_RPI_PICO_PIO_PIN_BY_NAME(idx, default, 0, scl_pins, 0),    \
	};                                                                                    \
	static struct pio_i2c_data pio_i2c_data_##idx;                                        \
	DEVICE_DT_INST_DEFINE(idx, pio_i2c_init, NULL, &pio_i2c_data_##idx,                   \
			      &pio_i2c_config_##idx, POST_KERNEL, CONFIG_I2C_INIT_PRIORITY,   \
			      &pio_i2c_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PIO_I2C_INIT)
