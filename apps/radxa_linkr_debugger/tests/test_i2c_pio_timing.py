#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Offline policy tests for the x1.2 PIO I2C Fast-mode implementation."""

from fractions import Fraction
from math import gcd
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[3]
APP_PATH = ROOT / "apps/radxa_linkr_debugger"
DRIVER_PATH = APP_PATH / "src/i2c_rpi_pico_pio.c"
OVERLAY_PATH = APP_PATH / "boards/rpi_pico2_rp2350a_m33.overlay"
BINDING_PATH = APP_PATH / "dts/bindings/i2c/raspberrypi,pico-pio-i2c.yaml"
PRJ_CONF_PATH = APP_PATH / "prj.conf"
HARDWARE_DOC_PATH = ROOT / "docs/reference/hardware-x1.2.md"
HIL_DOC_PATH = ROOT / "docs/testing/hil-functional-test-spec.md"
HIL_RESULT_PATH = (
    ROOT / "docs/testing/results/2026-10-20-pio-i2c-400khz-functional-hil.md"
)
PROJECT_SHELL_PATH = APP_PATH / "src/linkr_debugger_shell.c"
GENERATED_CONFIG_PATH = (
    ROOT / "build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/.config"
)
GENERATED_IMAGE_PATH = ROOT / "build/radxa_linkr_debugger/radxa_linkr_debugger"


def integer_define(source: str, name: str) -> int:
    match = re.search(
        rf"^#define\s+{re.escape(name)}\s+(0x[0-9a-fA-F]+|\d+)(?:U|ULL)?\s*$",
        source,
        re.MULTILINE,
    )
    if match is None:
        raise AssertionError(f"missing integer define {name}")
    return int(match.group(1), 0)


def instruction_duration(word: int) -> int:
    """Return base instruction plus delay cycles for one-bit optional side-set."""
    return 1 + ((word >> 8) & 0x7)


def side_set(word: int) -> int | None:
    if (word & 0x1000) == 0:
        return None
    return (word >> 11) & 1


def escape_sequence(source: str, function: str) -> list[str]:
    match = re.search(
        rf"static int {re.escape(function)}\(.*?const uint16_t instructions\[\] = \{{(.*?)\n\t\}};",
        source,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing escape sequence {function}")
    return re.findall(
        r"PIO_I2C_SC[01]_SD[01]|PIO_I2C_WAIT_SCL_PLACEHOLDER|pio_encode_mov",
        match.group(1),
    )


def encode_wait_gpio(gpio: int, delay: int = 0) -> int:
    return 0x2000 | (4 << 5) | (gpio & 0x1F) | (delay << 8)


class FractionalDivider:
    def __init__(self, integer: int, fraction: int, phase: int):
        phase_count = 256 // gcd(fraction, 256) if fraction else 1
        if not 0 <= phase < phase_count:
            raise ValueError("divider phase out of range")
        self.integer = integer
        self.fraction = fraction
        self.accumulator = (phase * fraction) & 0xFF

    def advance(self, cycles: int) -> int:
        ticks = 0
        for _ in range(cycles):
            ticks += self.integer
            self.accumulator += self.fraction
            if self.accumulator >= 256:
                ticks += 1
                self.accumulator -= 256
        return ticks


def simulate_program_high_window(
    words: list[int],
    indices: range,
    *,
    phase: int,
    stretch_ticks: int,
    synchronizer_ticks: int,
) -> int:
    divider = FractionalDivider(11, 8, phase)
    now = 0
    actual_high = None
    scl = 0

    for index in indices:
        word = words[index]
        side = side_set(word)
        if side == 1 and scl == 0:
            scl = 1
            actual_high = now + stretch_ticks
        elif side == 0 and scl == 1:
            if actual_high is None or now < actual_high:
                raise AssertionError("SCL fell before becoming physically high")
            return now - actual_high

        is_wait_gpio_high = (word & 0xE0E0) == 0x2080
        if is_wait_gpio_high:
            if actual_high is None:
                raise AssertionError("WAIT appeared before SCL release")
            visible_at = actual_high + synchronizer_ticks
            while now < visible_at:
                now += divider.advance(1)
        now += divider.advance(instruction_duration(word))

    raise AssertionError("modeled window did not contain an SCL falling edge")


def escape_state(token: str) -> tuple[int, int] | None:
    match = re.fullmatch(r"PIO_I2C_SC([01])_SD([01])", token)
    if match is None:
        return None
    return int(match.group(1)), int(match.group(2))


def simulate_escape_edges(
    tokens: list[str],
    *,
    phase: int,
    stretch_ticks: int,
    synchronizer_ticks: int,
    raw_level_cycles: int,
    wait_exec_cycles: int,
) -> dict[str, list[int]]:
    divider = FractionalDivider(11, 8, phase)
    now = 0
    state = None
    actual_high = None
    events: dict[str, list[int]] = {
        "high_windows": [],
        "repstart_setup": [],
        "stop_setup": [],
    }

    for token in tokens:
        next_state = escape_state(token)
        if next_state is not None:
            if state is not None:
                old_scl, old_sda = state
                scl, sda = next_state
                if old_scl == 0 and scl == 1:
                    actual_high = now + stretch_ticks
                if old_scl == 1 and scl == 0:
                    if actual_high is None or now < actual_high:
                        raise AssertionError("escape lowered SCL before actual high")
                    events["high_windows"].append(now - actual_high)
                if old_sda == 1 and sda == 0 and scl == 1:
                    if actual_high is None or now < actual_high:
                        raise AssertionError("REPSTART preceded actual SCL high")
                    events["repstart_setup"].append(now - actual_high)
                if old_sda == 0 and sda == 1 and scl == 1:
                    if actual_high is None or now < actual_high:
                        raise AssertionError("STOP preceded actual SCL high")
                    events["stop_setup"].append(now - actual_high)
            state = next_state
            now += divider.advance(raw_level_cycles)
            continue

        if token == "PIO_I2C_WAIT_SCL_PLACEHOLDER":
            if actual_high is None:
                raise AssertionError("escape WAIT preceded SCL release")
            visible_at = actual_high + synchronizer_ticks
            while now < visible_at:
                now += divider.advance(1)
            now += divider.advance(wait_exec_cycles)
            continue

        if token == "pio_encode_mov":
            now += divider.advance(3)
            continue

        raise AssertionError(f"unknown escape token: {token}")

    return events


class RecoveryStateMachine:
    """Minimal OSR/FIFO/internal-state model for the NAK cleanup boundary."""

    def __init__(self) -> None:
        self.enabled = True
        self.oe_forced_off = False
        self.autopull = True
        self.autopush = True
        self.tx_fifo = [0xF780, 0x2094, 0xFF80]
        self.rx_fifo = [0x12, 0x34]
        self.osr = 0x2094
        self.osr_count = 0
        self.isr = 0xA5
        self.isr_count = 7
        self.exec_stalled = True
        self.delay_counter = 5
        self.irq_wait = True
        self.irq = True
        self.pc = 16
        self.wrap_bottom = 17
        self.pins_released = False
        self.config = {
            "origin": 3,
            "wrap": (20, 25),
            "clkdiv": (11, 8),
            "out_pin": 21,
            "in_pin": 21,
            "sideset_pin": 20,
        }

    def _autopull_if_empty(self) -> None:
        if self.autopull and self.osr_count >= 16:
            if not self.tx_fifo:
                raise AssertionError("parser stalled waiting for a record header")
            self.osr = self.tx_fifo.pop(0)
            self.osr_count = 0

    def parser_out_x(self) -> int:
        self._autopull_if_empty()
        value = (self.osr >> 10) & 0x3F
        self.osr_count += 6
        return value

    def queue_stop_header(self, instruction_count: int) -> int:
        header = (instruction_count - 1) << 10
        self.tx_fifo.append(header)
        return header

    def old_drain_and_jump(self) -> None:
        # pio_sm_drain_tx_fifo() executes OUT NULL,32 with autopull enabled.
        while self.tx_fifo:
            self.osr_count = 32
            self._autopull_if_empty()
        self.enabled = False
        self.pins_released = True
        self.pc = self.wrap_bottom
        self.irq = False
        self.enabled = True

    def full_restart_and_resume(self) -> None:
        saved_autopull = self.autopull
        saved_autopush = self.autopush
        saved_config = dict(self.config)

        self.oe_forced_off = True
        self.enabled = False
        self.autopull = False
        self.autopush = False
        self.tx_fifo.clear()
        self.rx_fifo.clear()

        # pio_sm_restart(): counters/ISR/EXEC/delay/IRQ-wait clear, OSR data
        # remains. OUT NULL,32 with autopull disabled then marks that OSR empty.
        self.osr_count = 0
        self.isr = 0
        self.isr_count = 0
        self.exec_stalled = False
        self.delay_counter = 0
        self.irq_wait = False
        self.osr_count = 32

        self.pins_released = True
        self.pc = self.wrap_bottom
        self.irq = False
        self.autopull = saved_autopull
        self.autopush = saved_autopush
        self.config = saved_config
        self.oe_forced_off = False
        self.enabled = True


class PioI2cTimingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.driver = DRIVER_PATH.read_text(encoding="utf-8")
        cls.overlay = OVERLAY_PATH.read_text(encoding="utf-8")
        cls.binding = BINDING_PATH.read_text(encoding="utf-8")
        cls.prj_conf = PRJ_CONF_PATH.read_text(encoding="utf-8")
        cls.hardware_doc = HARDWARE_DOC_PATH.read_text(encoding="utf-8")
        cls.hil_doc = HIL_DOC_PATH.read_text(encoding="utf-8")
        cls.hil_result = HIL_RESULT_PATH.read_text(encoding="utf-8")
        cls.project_shell = PROJECT_SHELL_PATH.read_text(encoding="utf-8")

        program = re.search(
            r"RPI_PICO_PIO_DEFINE_PROGRAM\(i2c_pio,\s*(\d+),\s*(\d+),(.*?)\n\);",
            cls.driver,
            re.DOTALL,
        )
        if program is None:
            raise AssertionError("missing i2c_pio program")
        cls.wrap_target = int(program.group(1))
        cls.wrap = int(program.group(2))
        instruction_tokens = re.findall(
            r"^\s*(0x[0-9a-fA-F]{4}|PIO_I2C_INSTR_[A-Z0-9_]+)\s*,",
            program.group(3),
            re.MULTILINE,
        )
        cls.instructions = [
            int(token, 16)
            if token.startswith("0x")
            else integer_define(cls.driver, token)
            for token in instruction_tokens
        ]

    def trace_ack_path(
        self, *, pin_high: bool, nak_expected: bool, next_x: int
    ) -> tuple[int | None, list[int], str]:
        """Execute from ACK falling edge until the next SCL high or terminal wait."""
        pc = 16
        x = 0
        y = 1 if nak_expected else 0
        scl = 1
        cycles = 0
        pcs = []

        for _ in range(32):
            pcs.append(pc)
            word = self.instructions[pc]
            side = side_set(word)
            if side == 1 and scl == 0:
                return cycles, pcs, "scl-high"
            if side is not None:
                scl = side

            if pc == 1:
                return None, pcs, "irq-wait"

            cycles += instruction_duration(word)
            if pc == 16:
                pc = 0 if pin_high else 17
            elif pc == 0:
                branch = y != 0
                y = (y - 1) & 0xFFFFFFFF
                pc = 17 if branch else 1
            elif pc == 17:
                x = next_x
                pc = 18
            elif pc == 19:
                pc = 2 if x == 0 else 20
            elif pc == 2:
                x = 7
                pc = 3
            elif pc == 21:
                return None, pcs, "out-exec"
            else:
                pc += 1

        self.fail(f"ACK path did not terminate: {pcs}")

    def test_pioasm_machine_code_is_exact(self) -> None:
        # Generated with Pico SDK pioasm 2.2.0 from the documented 23-instruction
        # program. This catches stale labels as well as delay/side-set drift.
        self.assertEqual((self.wrap_target, self.wrap), (17, 22))
        self.assertEqual(
            self.instructions,
            [
                0x0091,
                0xC030,
                0xE027,
                0x6781,
                0xB142,
                0xB942,
                0x2480,
                0xB842,
                0x4701,
                0x1743,
                0x6781,
                0xB242,
                0xBA42,
                0x2380,
                0xBF42,
                0xB842,
                0x13C0,
                0x6026,
                0x6041,
                0x0022,
                0x6060,
                0x60F0,
                0x0055,
            ],
        )

        # Data clocks have 18 low and 16 high cycles.
        data_low = sum(
            instruction_duration(self.instructions[index]) for index in (9, 3, 4)
        )
        data_high = sum(
            instruction_duration(self.instructions[index]) for index in (5, 6, 7, 8)
        )
        ack_high = sum(
            instruction_duration(self.instructions[index])
            for index in (12, 13, 14, 15)
        )
        self.assertEqual(data_low, 18)
        self.assertEqual(data_high, 16)
        self.assertEqual(ack_high, 16)

        self.assertEqual(self.instructions[6], encode_wait_gpio(0, 4))
        self.assertEqual(self.instructions[13], encode_wait_gpio(0, 3))
        self.assertEqual(
            sum(instruction_duration(self.instructions[index]) for index in (7, 8)),
            9,
        )
        self.assertEqual(
            sum(instruction_duration(self.instructions[index]) for index in (14, 15)),
            9,
        )

    def test_runtime_program_waits_use_the_devicetree_scl_gpio(self) -> None:
        for scl_pin, gpio_base, expected_gpio in ((20, 0, 20), (23, 16, 7)):
            words = list(self.instructions)
            words[6] = encode_wait_gpio(scl_pin, 4)
            words[13] = encode_wait_gpio(scl_pin, 3)

            # pio_add_program() converts absolute source GPIO encoding to the
            # PIO instance's relative GPIO_BASE encoding.
            if gpio_base == 16:
                words[6] ^= 16
                words[13] ^= 16
            self.assertEqual(words[6] & 0x1F, expected_gpio)
            self.assertEqual(words[13] & 0x1F, expected_gpio)

            # OUT EXEC bypasses the program loader, so the escape encoder must
            # subtract GPIO_BASE directly.
            escape_wait = encode_wait_gpio(scl_pin - gpio_base)
            self.assertEqual(escape_wait & 0x1F, expected_gpio)

        prepare = self.driver[
            self.driver.index("static int pio_i2c_prepare_program") :
            self.driver.index("static const uint16_t pio_i2c_recovery_instructions")
        ]
        self.assertIn("pio_encode_wait_gpio(true, scl_pin)", prepare)
        encoder = self.driver[
            self.driver.index("static int pio_i2c_encode_exec_wait_scl") :
            self.driver.index("static int pio_i2c_queue_escape")
        ]
        self.assertIn("pio_encode_wait_gpio(true, scl_pin - gpio_base)", encoder)
        self.assertNotIn("gpio, 20", self.driver)

    def test_data_and_ack_high_windows_cover_all_fractional_phases(self) -> None:
        words = list(self.instructions)
        words[6] = encode_wait_gpio(20, 4)
        words[13] = encode_wait_gpio(20, 3)
        fast_mode_min_ticks = 90  # 600 ns at clk_sys=150 MHz

        for phase in range(32):
            # Explicit post-WAIT holds alone must have margin over 600 ns in
            # every 11+8/256 divider phase; an eight-cycle hold would fail.
            for indices in ((7, 8), (14, 15)):
                divider = FractionalDivider(11, 8, phase)
                post_wait_ticks = sum(
                    divider.advance(instruction_duration(words[index]))
                    for index in indices
                )
                self.assertGreater(post_wait_ticks, fast_mode_min_ticks)

            for stretch_ticks in (0, 1, 37, 1_000):
                for synchronizer_ticks in (0, 1, 2):
                    with self.subTest(
                        phase=phase,
                        stretch=stretch_ticks,
                        synchronizer=synchronizer_ticks,
                    ):
                        data_high = simulate_program_high_window(
                            words,
                            range(5, 10),
                            phase=phase,
                            stretch_ticks=stretch_ticks,
                            synchronizer_ticks=synchronizer_ticks,
                        )
                        ack_high = simulate_program_high_window(
                            words,
                            range(12, 17),
                            phase=phase,
                            stretch_ticks=stretch_ticks,
                            synchronizer_ticks=synchronizer_ticks,
                        )
                        self.assertGreater(data_high, fast_mode_min_ticks)
                        self.assertGreater(ack_high, fast_mode_min_ticks)

    def test_ack_success_and_nak_paths_are_traced_instruction_by_instruction(self) -> None:
        ack_low, pcs, terminal = self.trace_ack_path(
            pin_high=False, nak_expected=False, next_x=0
        )
        self.assertEqual(terminal, "scl-high")
        self.assertEqual(pcs, [16, 17, 18, 19, 2, 3, 4, 5])
        self.assertEqual(ack_low, 18)

        _, pcs, terminal = self.trace_ack_path(
            pin_high=True, nak_expected=False, next_x=0
        )
        self.assertEqual(terminal, "irq-wait")
        self.assertEqual(pcs, [16, 0, 1])

        _, pcs, terminal = self.trace_ack_path(
            pin_high=True, nak_expected=True, next_x=2
        )
        self.assertEqual(terminal, "out-exec")
        self.assertEqual(pcs, [16, 0, 17, 18, 19, 20, 21])

    def test_nak_cleanup_discards_prefetched_osr_and_partial_escape(self) -> None:
        old = RecoveryStateMachine()
        old.old_drain_and_jump()
        old.queue_stop_header(5)
        self.assertNotEqual(old.parser_out_x(), 4)
        self.assertNotEqual(old.osr, 0x1000)
        self.assertNotEqual(old.rx_fifo, [])

        recovered = RecoveryStateMachine()
        original_config = dict(recovered.config)
        recovered.full_restart_and_resume()
        self.assertEqual(recovered.tx_fifo, [])
        self.assertEqual(recovered.rx_fifo, [])
        self.assertEqual(recovered.osr_count, 32)
        self.assertEqual((recovered.isr, recovered.isr_count), (0, 0))
        self.assertFalse(recovered.exec_stalled)
        self.assertEqual(recovered.delay_counter, 0)
        self.assertFalse(recovered.irq_wait)
        self.assertFalse(recovered.irq)
        self.assertTrue(recovered.pins_released)
        self.assertTrue(recovered.autopull)
        self.assertTrue(recovered.autopush)
        self.assertEqual(recovered.pc, recovered.wrap_bottom)
        self.assertEqual(recovered.config, original_config)

        stop_header = recovered.queue_stop_header(5)
        self.assertEqual(stop_header, 0x1000)
        self.assertEqual(recovered.parser_out_x(), 4)
        self.assertEqual(recovered.osr, stop_header)

        restart = self.driver[
            self.driver.index("static void pio_i2c_restart_sm_released") :
            self.driver.index("static void pio_i2c_resume_after_error")
        ]
        ordered_calls = (
            "pio_sm_set_enabled(pio, sm, false)",
            "hw_clear_bits(&pio->sm[sm].shiftctrl",
            "pio_sm_clear_fifos(pio, sm)",
            "pio_sm_restart(pio, sm)",
            "pio_sm_clkdiv_restart(pio, sm)",
            "pio_encode_out(pio_null, 32)",
            "pio_sm_set_pins_with_mask",
            "pio_sm_set_pindirs_with_mask",
            "pio_encode_jmp(wrap_bottom)",
            "pio_interrupt_clear(pio, sm)",
            "pio->sm[sm].shiftctrl = shiftctrl",
        )
        positions = [restart.index(call) for call in ordered_calls]
        self.assertEqual(positions, sorted(positions))
        self.assertNotIn("pio_sm_drain_tx_fifo", restart)
        self.assertNotIn("pio_sm_init", restart)

        resume = self.driver[
            self.driver.index("static void pio_i2c_resume_after_error") :
            self.driver.index("static inline int pio_i2c_put_or_err")
        ]
        oe_off = resume.index("GPIO_OVERRIDE_LOW")
        reset = resume.index("pio_i2c_restart_sm_released")
        oe_restore = resume.index("GPIO_OVERRIDE_INVERT")
        enable = resume.index("pio_sm_set_enabled")
        self.assertLess(oe_off, reset)
        self.assertLess(reset, oe_restore)
        self.assertLess(oe_restore, enable)

        def cleanup_result(original_error: int, cleanup_error: int) -> int:
            return cleanup_error if cleanup_error < 0 else original_error

        self.assertEqual(cleanup_result(-5, 0), -5)
        self.assertEqual(cleanup_result(-5, -110), -110)
        transfer = self.driver[
            self.driver.index("static int pio_i2c_transfer_once") :
            self.driver.index(
                "static int pio_i2c_transfer(",
                self.driver.index("static int pio_i2c_transfer_once"),
            )
        ]
        self.assertIn("err == -EIO && pio_i2c_check_error(pio, sm)", transfer)
        self.assertIsNotNone(
            re.search(
                r"int cleanup_err = pio_i2c_cleanup_after_irq\(dev\);\s*"
                r"if \(cleanup_err < 0\) \{\s*err = cleanup_err;",
                transfer,
                re.DOTALL,
            )
        )

    def test_escape_timing_and_open_drain_policy_remain_intact(self) -> None:
        self.assertEqual(
            escape_sequence(self.driver, "pio_i2c_start"),
            ["PIO_I2C_SC1_SD0", "PIO_I2C_SC0_SD0", "pio_encode_mov"],
        )
        self.assertEqual(
            escape_sequence(self.driver, "pio_i2c_repstart"),
            [
                "PIO_I2C_SC0_SD1",
                "PIO_I2C_SC1_SD1",
                "PIO_I2C_WAIT_SCL_PLACEHOLDER",
                "PIO_I2C_SC1_SD1",
                "PIO_I2C_SC1_SD0",
                "PIO_I2C_SC0_SD0",
                "pio_encode_mov",
            ],
        )
        self.assertEqual(
            escape_sequence(self.driver, "pio_i2c_stop"),
            [
                "PIO_I2C_SC0_SD0",
                "PIO_I2C_SC1_SD0",
                "PIO_I2C_WAIT_SCL_PLACEHOLDER",
                "PIO_I2C_SC1_SD0",
                "PIO_I2C_SC1_SD1",
            ],
        )
        for name, word in {
            "PIO_I2C_SC0_SD0": "f780",
            "PIO_I2C_SC0_SD1": "f781",
            "PIO_I2C_SC1_SD0": "ff80",
            "PIO_I2C_SC1_SD1": "ff81",
        }.items():
            self.assertRegex(self.driver, rf"#define {name} 0x{word}U")

        init_start = self.driver.index("static int pio_i2c_init")
        init_end = self.driver.index("static int pio_i2c_recover_locked", init_start)
        init = self.driver[init_start:init_end]
        pinctrl = init.index("pinctrl_apply_state")
        sda_oe_disabled = [
            match.start()
            for match in re.finditer(
                r"gpio_set_oeover\(config->sda_pin, GPIO_OVERRIDE_LOW\)", init
            )
        ]
        scl_oe_disabled = [
            match.start()
            for match in re.finditer(
                r"gpio_set_oeover\(config->scl_pin, GPIO_OVERRIDE_LOW\)", init
            )
        ]
        sda_pulls_disabled = init.index("gpio_disable_pulls(config->sda_pin)")
        scl_pulls_disabled = init.index("gpio_disable_pulls(config->scl_pin)")
        failure_check = init.index("if (ret < 0)", pinctrl)
        self.assertGreaterEqual(len(sda_oe_disabled), 2)
        self.assertGreaterEqual(len(scl_oe_disabled), 2)
        self.assertLess(max(sda_oe_disabled[0], scl_oe_disabled[0]), sda_pulls_disabled)
        self.assertLess(max(sda_pulls_disabled, scl_pulls_disabled), pinctrl)
        self.assertLess(pinctrl, min(sda_oe_disabled[1], scl_oe_disabled[1]))
        self.assertLess(max(sda_oe_disabled[1], scl_oe_disabled[1]), failure_check)
        self.assertEqual(self.overlay.count("bias-disable;"), 2)
        self.assertEqual(self.overlay.count("input-schmitt-enable;"), 2)
        self.assertNotIn("bias-pull-up", self.overlay)
        self.assertNotIn("bias-pull-down", self.overlay)

        for group in ("sda_pins", "scl_pins"):
            match = re.search(rf"{group} \{{(.*?)\n\t\t\}};", self.overlay, re.DOTALL)
            self.assertIsNotNone(match)
            self.assertIn("bias-disable;", match.group(1))
            self.assertIn("input-enable;", match.group(1))
            self.assertIn("input-schmitt-enable;", match.group(1))

        pre_pinctrl = re.sub(r"/\*.*?\*/", "", init[:pinctrl], flags=re.DOTALL)
        self.assertNotRegex(pre_pinctrl, r"\bgpio_init\s*\(")
        self.assertNotRegex(pre_pinctrl, r"\bgpio_set_function\s*\(")

        rearm_start = self.driver.index("static void pio_i2c_rearm")
        rearm_end = self.driver.index(
            "static int pio_i2c_cleanup_after_irq", rearm_start
        )
        rearm = self.driver[rearm_start:rearm_end]
        self.assertLess(
            rearm.index("pio_i2c_reset_and_release"),
            rearm.index("GPIO_OVERRIDE_INVERT"),
        )

    def test_150mhz_400khz_divider_and_fast_mode_bounds(self) -> None:
        data_low = sum(
            instruction_duration(self.instructions[index]) for index in (9, 3, 4)
        )
        data_high = sum(
            instruction_duration(self.instructions[index]) for index in (5, 6, 7, 8)
        )
        ack_low, _, _ = self.trace_ack_path(
            pin_high=False, nak_expected=False, next_x=0
        )
        self.assertIsNotNone(ack_low)
        cycles_per_bit = data_low + data_high
        divider_scale = integer_define(self.driver, "PIO_I2C_CLKDIV_SCALE")
        sys_hz = 150_000_000
        requested_hz = 400_000
        denominator = cycles_per_bit * requested_hz
        scaled_divider = (sys_hz * divider_scale + denominator - 1) // denominator
        effective_hz = (sys_hz * divider_scale) // (cycles_per_bit * scaled_divider)

        self.assertEqual(cycles_per_bit, 34)
        self.assertEqual(
            (scaled_divider // divider_scale, scaled_divider % divider_scale),
            (11, 8),
        )
        self.assertEqual(effective_hz, 399_933)
        self.assertLessEqual(effective_hz, requested_hz)

        div_int = scaled_divider // divider_scale
        shortest_cycle_ns = Fraction(div_int * 1_000_000_000, sys_hz)
        longest_cycle_ns = Fraction((div_int + 1) * 1_000_000_000, sys_hz)
        minimums_ns = {
            "data-low": (data_low, 1_300),
            "data-high": (data_high, 600),
            "data-setup": (
                sum(instruction_duration(self.instructions[index]) for index in (3, 4)),
                100,
            ),
            "ack-low": (ack_low, 1_300),
            "ack-high": (
                sum(
                    instruction_duration(self.instructions[index])
                    for index in (12, 13, 14, 15)
                ),
                600,
            ),
        }
        raw_level_cycles = (
            instruction_duration(integer_define(self.driver, "PIO_I2C_SC0_SD0"))
            + instruction_duration(self.instructions[21])
            + instruction_duration(self.instructions[22])
        )
        minimums_ns.update(
            {
                "start-hold": (raw_level_cycles, 600),
                "repstart-setup": (raw_level_cycles, 600),
                "stop-setup": (raw_level_cycles, 600),
            }
        )
        for name, (cycles, required_ns) in minimums_ns.items():
            with self.subTest(name=name):
                actual_ns = cycles * shortest_cycle_ns
                self.assertGreaterEqual(actual_ns, required_ns)

        data_valid_ns = instruction_duration(self.instructions[9]) * longest_cycle_ns
        self.assertLessEqual(data_valid_ns, 900)
        ack_valid_ns = instruction_duration(self.instructions[9]) * longest_cycle_ns
        self.assertLessEqual(ack_valid_ns, 900)
        self.assertGreaterEqual(
            integer_define(self.driver, "PIO_I2C_IDLE_SAMPLE_MIN_USEC") * 1_000,
            1_300,
        )

    def test_stop_and_repstart_wait_for_actual_scl_high(self) -> None:
        sequences = {
            "stop": escape_sequence(self.driver, "pio_i2c_stop"),
            "repstart": escape_sequence(self.driver, "pio_i2c_repstart"),
        }
        minimum_ticks = 90
        raw_level_cycles = (
            instruction_duration(integer_define(self.driver, "PIO_I2C_SC0_SD0"))
            + instruction_duration(self.instructions[21])
            + instruction_duration(self.instructions[22])
        )
        wait_exec_cycles = (
            instruction_duration(encode_wait_gpio(20))
            + instruction_duration(self.instructions[21])
            + instruction_duration(self.instructions[22])
        )
        self.assertEqual((raw_level_cycles, wait_exec_cycles), (10, 3))

        for name, tokens in sequences.items():
            for index, token in enumerate(tokens[:-1]):
                state = escape_state(token)
                previous = escape_state(tokens[index - 1]) if index > 0 else None
                if state is not None and previous is not None:
                    if previous[0] == 0 and state[0] == 1:
                        self.assertEqual(
                            tokens[index + 1], "PIO_I2C_WAIT_SCL_PLACEHOLDER"
                        )
                        self.assertEqual(escape_state(tokens[index + 2]), state)

            for phase in range(32):
                for stretch_ticks in (0, 1, 37, 1_000):
                    for synchronizer_ticks in (0, 1, 2):
                        events = simulate_escape_edges(
                            tokens,
                            phase=phase,
                            stretch_ticks=stretch_ticks,
                            synchronizer_ticks=synchronizer_ticks,
                            raw_level_cycles=raw_level_cycles,
                            wait_exec_cycles=wait_exec_cycles,
                        )
                        setup_key = (
                            "stop_setup" if name == "stop" else "repstart_setup"
                        )
                        self.assertEqual(len(events[setup_key]), 1)
                        self.assertGreater(events[setup_key][0], minimum_ticks)
                        if name == "repstart":
                            self.assertEqual(len(events["high_windows"]), 1)
                            self.assertGreater(
                                events["high_windows"][0], minimum_ticks
                            )

    def test_recovery_waits_every_high_and_models_fifo_timeout(self) -> None:
        match = re.search(
            r"static const uint16_t pio_i2c_recovery_instructions\[\] = \{(.*?)\n\};",
            self.driver,
            re.DOTALL,
        )
        self.assertIsNotNone(match)
        tokens = re.findall(
            r"PIO_I2C_SC[01]_SD[01]|PIO_I2C_WAIT_SCL_PLACEHOLDER",
            match.group(1),
        )
        states = [token for token in tokens if escape_state(token) is not None]
        waits = [token for token in tokens if token == "PIO_I2C_WAIT_SCL_PLACEHOLDER"]
        self.assertEqual((len(tokens), len(states), len(waits)), (51, 41, 10))
        raw_level_cycles = (
            instruction_duration(integer_define(self.driver, "PIO_I2C_SC0_SD0"))
            + instruction_duration(self.instructions[21])
            + instruction_duration(self.instructions[22])
        )
        wait_exec_cycles = (
            instruction_duration(encode_wait_gpio(20))
            + instruction_duration(self.instructions[21])
            + instruction_duration(self.instructions[22])
        )
        self.assertEqual((raw_level_cycles, wait_exec_cycles), (10, 3))

        pulse = [
            "PIO_I2C_SC0_SD1",
            "PIO_I2C_SC0_SD1",
            "PIO_I2C_SC1_SD1",
            "PIO_I2C_WAIT_SCL_PLACEHOLDER",
            "PIO_I2C_SC1_SD1",
        ]
        for pulse_index in range(9):
            self.assertEqual(tokens[pulse_index * 5 : pulse_index * 5 + 5], pulse)
        self.assertEqual(
            tokens[-6:],
            [
                "PIO_I2C_SC0_SD0",
                "PIO_I2C_SC0_SD0",
                "PIO_I2C_SC1_SD0",
                "PIO_I2C_WAIT_SCL_PLACEHOLDER",
                "PIO_I2C_SC1_SD0",
                "PIO_I2C_SC1_SD1",
            ],
        )

        for index, token in enumerate(tokens[:-2]):
            state = escape_state(token)
            previous = escape_state(tokens[index - 1]) if index > 0 else None
            if state is not None and previous is not None:
                if previous[0] == 0 and state[0] == 1:
                    self.assertEqual(
                        tokens[index + 1], "PIO_I2C_WAIT_SCL_PLACEHOLDER"
                    )
                    self.assertEqual(escape_state(tokens[index + 2]), state)

        scaled_divider = 2_824
        sm_hz = Fraction(150_000_000 * 256, scaled_divider)
        recovery_hz = sm_hz / 43
        fastest_recovery_hz = Fraction(150_000_000, 11 * 43)

        self.assertLessEqual(fastest_recovery_hz, 400_000)
        self.assertAlmostEqual(float(recovery_hz), 316_226.365, places=3)

        for phase in range(32):
            for stretch_ticks in (0, 1, 37, 1_000):
                for synchronizer_ticks in (0, 1, 2):
                    events = simulate_escape_edges(
                        tokens,
                        phase=phase,
                        stretch_ticks=stretch_ticks,
                        synchronizer_ticks=synchronizer_ticks,
                        raw_level_cycles=raw_level_cycles,
                        wait_exec_cycles=wait_exec_cycles,
                    )
                    self.assertEqual(len(events["high_windows"]), 9)
                    self.assertEqual(len(events["stop_setup"]), 1)
                    self.assertTrue(
                        all(window > 90 for window in events["high_windows"])
                    )
                    self.assertGreater(events["stop_setup"][0], 90)

        escape_setup_cycles = sum(
            instruction_duration(self.instructions[index])
            for index in (17, 18, 19, 20)
        )
        recovery_sm_cycles = (
            escape_setup_cycles
            + len(states) * raw_level_cycles
            + len(waits) * wait_exec_cycles
        )
        recovery_transfer_clocks = (recovery_sm_cycles + 34 - 1) // 34
        idle_clocks = (5 * 400_000 + 1_000_000 - 1) // 1_000_000
        self.assertEqual((recovery_sm_cycles, recovery_transfer_clocks), (444, 14))
        self.assertEqual(recovery_transfer_clocks + idle_clocks, 16)

        # With SCL stuck low, OUT EXEC stalls at the first WAIT. The producer
        # can fill only the four-word TX FIFO, so this 52-word record must take
        # the pio_i2c_put16 deadline path rather than spin forever.
        first_wait = tokens.index("PIO_I2C_WAIT_SCL_PLACEHOLDER")
        record_words = 1 + len(tokens)
        consumed_through_wait = 1 + first_wait + 1
        self.assertLess(consumed_through_wait + 4, record_words)

        put16 = self.driver[
            self.driver.index("static inline int pio_i2c_put16") :
            self.driver.index("static void pio_i2c_resume_after_error")
        ]
        self.assertIn("pio_i2c_status(data, pio, sm)", put16)
        status = self.driver[
            self.driver.index("static int pio_i2c_status") :
            self.driver.index("static inline int pio_i2c_put16")
        ]
        self.assertIn("pio_i2c_expired(data)", status)
        recover = self.driver[
            self.driver.index("static int pio_i2c_recover_locked(const struct device *dev)\n{") :
            self.driver.index("static int pio_i2c_recover_bus", self.driver.index("static int pio_i2c_recover_locked(const struct device *dev)\n{"))
        ]
        self.assertIsNotNone(
            re.search(
                r"if \(ret == 0\).*?else \{\s*pio_i2c_fail_close\(dev\);",
                recover,
                re.DOTALL,
            )
        )
        cleanup = self.driver[
            self.driver.index("static int pio_i2c_cleanup_after_irq") :
            self.driver.index("static int pio_i2c_recover_bus", self.driver.index("static int pio_i2c_cleanup_after_irq"))
        ]
        self.assertRegex(cleanup, r"if \(ret < 0\) \{\s*pio_i2c_fail_close\(dev\);")
        transfer = self.driver[
            self.driver.index("static int pio_i2c_transfer_once") :
            self.driver.index("static int pio_i2c_transfer(", self.driver.index("static int pio_i2c_transfer_once"))
        ]
        self.assertIsNotNone(
            re.search(
                r"if \(err != 0\).*?else \{\s*pio_i2c_fail_close\(dev\);",
                transfer,
                re.DOTALL,
            )
        )

    def test_dt_defaults_to_200khz_with_400khz_capability_ceiling(self) -> None:
        self.assertRegex(self.overlay, r"clock-frequency\s*=\s*<200000>;")
        self.assertNotRegex(self.overlay, r"clock-frequency\s*=\s*<400000>;")
        self.assertRegex(self.binding, r"(?m)^\s+max:\s+400000\s*$")
        self.assertEqual(integer_define(self.driver, "PIO_I2C_MAX_REQUEST_HZ"), 400_000)

    def test_production_config_forbids_generic_mutation_shells(self) -> None:
        forbidden_shells = (
            "CONFIG_GPIO_SHELL",
            "CONFIG_DEVMEM_SHELL",
            "CONFIG_I2C_SHELL",
            "CONFIG_DEVICE_SHELL",
            "CONFIG_KERNEL_SHELL",
        )
        for symbol in forbidden_shells:
            with self.subTest(source_symbol=symbol):
                assignments = re.findall(
                    rf"^{symbol}=(.*)$", self.prj_conf, re.MULTILINE
                )
                self.assertEqual(assignments, ["n"])
        self.assertRegex(self.prj_conf, r"(?m)^CONFIG_GPIO=y$")
        self.assertRegex(self.prj_conf, r"(?m)^CONFIG_SHELL=y$")

        for config_path in APP_PATH.rglob("*.conf"):
            with self.subTest(config=str(config_path.relative_to(ROOT))):
                config = config_path.read_text(encoding="utf-8")
                for symbol in forbidden_shells:
                    self.assertNotRegex(config, rf"(?m)^{symbol}=y$")

        for command, registration_macro in (
            ("bootloader", "SHELL_CMD_REGISTER"),
            ("reboot", "SHELL_CMD_ARG_REGISTER"),
            ("config", "SHELL_CMD_REGISTER"),
            ("tf_wp", "SHELL_CMD_REGISTER"),
            ("task", "SHELL_CMD_REGISTER"),
            ("ip", "SHELL_CMD_REGISTER"),
            ("pd", "SHELL_CMD_REGISTER"),
        ):
            with self.subTest(project_command=command):
                self.assertRegex(
                    self.project_shell,
                    rf"{registration_macro}\({command},",
                )

    def test_generated_config_omits_generic_mutation_shells(self) -> None:
        if not GENERATED_CONFIG_PATH.exists():
            self.skipTest("run after make firmware to inspect generated .config")

        generated = GENERATED_CONFIG_PATH.read_text(encoding="utf-8")
        forbidden_shells = (
            "GPIO_SHELL",
            "DEVMEM_SHELL",
            "I2C_SHELL",
            "DEVICE_SHELL",
            "KERNEL_SHELL",
        )
        for symbol in forbidden_shells:
            with self.subTest(generated_symbol=symbol):
                self.assertNotRegex(generated, rf"(?m)^CONFIG_{symbol}=y$")
                self.assertIn(f"# CONFIG_{symbol} is not set", generated)

        forbidden_objects = (
            "gpio_shell.c.obj",
            "devmem_service.c.obj",
            "i2c_shell.c.obj",
            "device_service.c.obj",
            "kernel_shell.c.obj",
        )
        object_names = {
            path.name for path in GENERATED_IMAGE_PATH.rglob("*.c.obj")
        }
        for object_name in forbidden_objects:
            with self.subTest(object=object_name):
                self.assertNotIn(object_name, object_names)

    def test_cross_domain_electrical_boundaries_are_documented(self) -> None:
        required_hardware_contracts = (
            "no board-mounted external SCL/SDA pull-up",
            "7/10/15 kΩ internal pull-ups",
            "does not identify the pull-up rail",
            "`VS + 0.3 V`",
            "`VS = 0 V`",
            "6 V absolute",
            "32–86 kΩ",
            "5 V TVS",
            "must never enable it",
            "200 kHz by default",
            "4.7 kΩ fly-wire pull-up",
            "2.812/3.197/3.579 kΩ",
            "`tr = 81.0–103.1 ns`",
            "0.922–1.174 mA",
            "`Cbmax = 75.33 pF`",
            "`Cbmax = 98.94 pF`",
            "64.94 pF",
            "34 pF fixture capacitance",
            "50 ns",
        )
        for contract in required_hardware_contracts:
            with self.subTest(hardware_contract=contract):
                self.assertIn(contract, self.hardware_doc)
        self.assertNotRegex(self.hardware_doc, r"2\.2\s*kΩ|2\.2\s*kOhm")

        required_hil_contracts = (
            "RP2350 internal RPU 为 32–86 kΩ",
            "CH224A CFG2/CFG3 虽有 7/10/15 kΩ internal",
            "`VS + 0.3 V`",
            "`VS = 0 V`",
            "SDA 的绝对最大值虽为 6 V",
            "ESD5451N 是 5 V TVS",
            "200 kHz 默认频率本身不消除上述风险",
            "4.7 kΩ 飞线上拉到板载 VCC_3V3",
            "2.812/3.197/3.579 kΩ",
            "`tr = 81.0–103.1 ns`",
            "0.922–1.174 mA",
            "`Cbmax = 75.33 pF`",
            "`Cbmax = 98.94 pF`",
            "64.94 pF",
            "不执行单边掉电",
            "50 ns spike filter",
        )
        for contract in required_hil_contracts:
            with self.subTest(hil_contract=contract):
                self.assertIn(contract, self.hil_doc)

        required_result_contracts = (
            "Post-run fixture evidence correction",
            "user/operator corrected",
            "not a new HIL execution",
            "4.7 kOhm flying-wire pull-up",
            "2.812/3.197/3.579 kOhm",
            "81.0–103.1 ns",
            "0.922–1.174 mA",
            "`Cbmax = 75.33 pF`",
            "`Cbmax = 98.94 pF`",
            "64.94 pF",
        )
        for contract in required_result_contracts:
            with self.subTest(hil_result_contract=contract):
                self.assertIn(contract, self.hil_result)

        fixture_docs = "\n".join(
            (self.hardware_doc, self.hil_doc, self.hil_result)
        )
        self.assertNotRegex(
            fixture_docs,
            r"10\s*k(?:Ω|Ohm)\s+(?:fly|flying|飞线)|"
            r"10\s*k(?:Ω|Ohm)\s+external",
        )

    def test_corrected_fixture_calculations(self) -> None:
        external_ohms = 4_700.0
        internal_ohms = (7_000.0, 10_000.0, 15_000.0)
        capacitance_pf = 34.0
        rise_factor = 0.8473
        effective_ohms = tuple(
            external_ohms * internal / (external_ohms + internal)
            for internal in internal_ohms
        )

        self.assertEqual(
            tuple(round(value / 1_000, 3) for value in effective_ohms),
            (2.812, 3.197, 3.579),
        )
        rise_ns = tuple(
            rise_factor * value * capacitance_pf * 1e-3
            for value in effective_ohms
        )
        self.assertEqual((round(min(rise_ns), 1), round(max(rise_ns), 1)),
                         (81.0, 103.1))
        current_ma = tuple(3.3 / value * 1_000 for value in effective_ohms)
        self.assertEqual(
            (round(min(current_ma), 3), round(max(current_ma), 3)),
            (0.922, 1.174),
        )

        external_rise_ns = rise_factor * external_ohms * capacitance_pf * 1e-3
        external_cbmax_pf = 300e-9 / (rise_factor * external_ohms) * 1e12
        parallel_cbmax_pf = 300e-9 / (rise_factor * max(effective_ohms)) * 1e12
        self.assertEqual(round(external_rise_ns, 1), 135.4)
        self.assertEqual(round(external_cbmax_pf, 2), 75.33)
        self.assertEqual(round(parallel_cbmax_pf, 2), 98.94)
        self.assertEqual(round(parallel_cbmax_pf - capacitance_pf, 2), 64.94)

    def test_read_final_nack_and_write_ack_policy_remains_explicit(self) -> None:
        write_start = self.driver.index("\t\tif (!reading) {")
        read_start = self.driver.index("\n\t\t} else {", write_start)
        write_path = self.driver[write_start:read_start]
        read_and_later = self.driver[read_start:]
        self.assertIn("(msg->buf[n] << PIO_I2C_DATA_LSB) | 1U", write_path)
        self.assertNotIn("PIO_I2C_FINAL_LSB", write_path)
        self.assertIn(
            "(tx_remain == 0U) ? (1U << PIO_I2C_FINAL_LSB) : 0U",
            read_and_later,
        )
        self.assertIn(
            "(tx_remain == 0U) ? (1U << PIO_I2C_NAK_LSB) : 0U",
            read_and_later,
        )


if __name__ == "__main__":
    unittest.main()
