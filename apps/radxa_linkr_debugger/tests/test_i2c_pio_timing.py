#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Offline policy tests for the x1.2 PIO I2C Fast-mode implementation."""

from fractions import Fraction
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
    return re.findall(r"PIO_I2C_SC[01]_SD[01]|pio_encode_mov", match.group(1))


class PioI2cTimingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.driver = DRIVER_PATH.read_text(encoding="utf-8")
        cls.overlay = OVERLAY_PATH.read_text(encoding="utf-8")
        cls.binding = BINDING_PATH.read_text(encoding="utf-8")
        cls.prj_conf = PRJ_CONF_PATH.read_text(encoding="utf-8")
        cls.hardware_doc = HARDWARE_DOC_PATH.read_text(encoding="utf-8")
        cls.hil_doc = HIL_DOC_PATH.read_text(encoding="utf-8")
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
        pc = 13
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
            if pc == 13:
                pc = 0 if pin_high else 14
            elif pc == 0:
                branch = y != 0
                y = (y - 1) & 0xFFFFFFFF
                pc = 14 if branch else 1
            elif pc == 14:
                x = next_x
                pc = 15
            elif pc == 16:
                pc = 2 if x == 0 else 17
            elif pc == 2:
                x = 7
                pc = 3
            elif pc == 18:
                return None, pcs, "out-exec"
            else:
                pc += 1

        self.fail(f"ACK path did not terminate: {pcs}")

    def test_pioasm_machine_code_is_exact(self) -> None:
        # Generated with Pico SDK pioasm 2.2.0 from the documented 20-instruction
        # program. This catches stale labels as well as delay/side-set drift.
        self.assertEqual((self.wrap_target, self.wrap), (14, 19))
        self.assertEqual(
            self.instructions,
            [
                0x008E,
                0xC030,
                0xE027,
                0x6781,
                0xB142,
                0xBA42,
                0x2494,
                0x4701,
                0x1743,
                0x6781,
                0xB242,
                0xBF42,
                0x2794,
                0x13C0,
                0x6026,
                0x6041,
                0x0022,
                0x6060,
                0x60F0,
                0x0052,
            ],
        )

        # Data clocks have 18 low and 16 high cycles.
        data_low = sum(
            instruction_duration(self.instructions[index]) for index in (8, 3, 4)
        )
        data_high = sum(
            instruction_duration(self.instructions[index]) for index in (5, 6, 7)
        )
        ack_high = sum(
            instruction_duration(self.instructions[index]) for index in (11, 12)
        )
        self.assertEqual(data_low, 18)
        self.assertEqual(data_high, 16)
        self.assertEqual(ack_high, 16)

        # Both high phases release SCL before an absolute GPIO20 wait. A
        # stretched SCL therefore delays sampling/falling instead of shortening
        # the externally observed high phase.
        self.assertEqual(self.instructions[6], 0x2494)
        self.assertEqual(self.instructions[12], 0x2794)

    def test_ack_success_and_nak_paths_are_traced_instruction_by_instruction(self) -> None:
        ack_low, pcs, terminal = self.trace_ack_path(
            pin_high=False, nak_expected=False, next_x=0
        )
        self.assertEqual(terminal, "scl-high")
        self.assertEqual(pcs, [13, 14, 15, 16, 2, 3, 4, 5])
        self.assertEqual(ack_low, 18)

        _, pcs, terminal = self.trace_ack_path(
            pin_high=True, nak_expected=False, next_x=0
        )
        self.assertEqual(terminal, "irq-wait")
        self.assertEqual(pcs, [13, 0, 1])

        _, pcs, terminal = self.trace_ack_path(
            pin_high=True, nak_expected=True, next_x=2
        )
        self.assertEqual(terminal, "out-exec")
        self.assertEqual(pcs, [13, 0, 14, 15, 16, 17, 18])

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
                "PIO_I2C_SC1_SD0",
                "PIO_I2C_SC0_SD0",
                "pio_encode_mov",
            ],
        )
        self.assertEqual(
            escape_sequence(self.driver, "pio_i2c_stop"),
            ["PIO_I2C_SC0_SD0", "PIO_I2C_SC1_SD0", "PIO_I2C_SC1_SD1"],
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
        self.assertNotIn("bias-pull-up", self.overlay)

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
            instruction_duration(self.instructions[index]) for index in (8, 3, 4)
        )
        data_high = sum(
            instruction_duration(self.instructions[index]) for index in (5, 6, 7)
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
                sum(instruction_duration(self.instructions[index]) for index in (11, 12)),
                600,
            ),
        }
        raw_level_cycles = (
            instruction_duration(integer_define(self.driver, "PIO_I2C_SC0_SD0"))
            + instruction_duration(self.instructions[18])
            + instruction_duration(self.instructions[19])
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

        data_valid_ns = instruction_duration(self.instructions[8]) * longest_cycle_ns
        self.assertLessEqual(data_valid_ns, 900)
        ack_valid_ns = instruction_duration(self.instructions[8]) * longest_cycle_ns
        self.assertLessEqual(ack_valid_ns, 900)
        self.assertGreaterEqual(
            integer_define(self.driver, "PIO_I2C_IDLE_SAMPLE_MIN_USEC") * 1_000,
            1_300,
        )

    def test_recovery_is_slower_than_normal_bus(self) -> None:
        match = re.search(
            r"static const uint16_t pio_i2c_recovery_instructions\[\] = \{(.*?)\n\};",
            self.driver,
            re.DOTALL,
        )
        self.assertIsNotNone(match)
        states = re.findall(r"PIO_I2C_SC[01]_SD[01]", match.group(1))
        self.assertEqual(len(states), 41)

        raw_level_cycles = (
            instruction_duration(integer_define(self.driver, "PIO_I2C_SC0_SD0"))
            + instruction_duration(self.instructions[18])
            + instruction_duration(self.instructions[19])
        )
        self.assertEqual(raw_level_cycles, 10)
        recovery_cycles = 4 * raw_level_cycles
        scaled_divider = 2_824
        sm_hz = Fraction(150_000_000 * 256, scaled_divider)
        recovery_hz = sm_hz / recovery_cycles
        fastest_recovery_hz = Fraction(150_000_000, 11 * recovery_cycles)

        self.assertEqual(recovery_cycles, 40)
        self.assertLessEqual(fastest_recovery_hz, 400_000)
        self.assertAlmostEqual(float(recovery_hz), 339_943.343, places=3)

        scl = {name: int(name[10]) for name in set(states)}
        sda = {name: int(name[14]) for name in set(states)}
        segments: list[tuple[int, int]] = []
        for state in states:
            if segments and segments[-1][0] == scl[state]:
                segments[-1] = (scl[state], segments[-1][1] + raw_level_cycles)
            else:
                segments.append((scl[state], raw_level_cycles))

        self.assertEqual([level for level, _ in segments], [0, 1] * 10)
        low_cycles = [cycles for level, cycles in segments if level == 0]
        high_cycles = [cycles for level, cycles in segments if level == 1]
        self.assertEqual(low_cycles, [20] * 10)
        self.assertEqual(high_cycles, [20] * 9 + [30])
        shortest_cycle_ns = Fraction(11 * 1_000_000_000, 150_000_000)
        for low, high in zip(low_cycles, high_cycles):
            self.assertGreaterEqual(low * shortest_cycle_ns, 1_300)
            self.assertGreaterEqual(high * shortest_cycle_ns, 600)
            self.assertLessEqual(
                Fraction(150_000_000, 11 * (low + high)), 400_000
            )

        self.assertEqual(
            states[-5:],
            [
                "PIO_I2C_SC0_SD0",
                "PIO_I2C_SC0_SD0",
                "PIO_I2C_SC1_SD0",
                "PIO_I2C_SC1_SD0",
                "PIO_I2C_SC1_SD1",
            ],
        )
        stop_index = len(states) - 1
        self.assertEqual((scl[states[stop_index]], sda[states[stop_index]]), (1, 1))
        stop_setup_cycles = 0
        for state in reversed(states[:stop_index]):
            if (scl[state], sda[state]) != (1, 0):
                break
            stop_setup_cycles += raw_level_cycles
        self.assertEqual(stop_setup_cycles, 20)
        self.assertGreaterEqual(stop_setup_cycles * shortest_cycle_ns, 600)

        escape_setup_cycles = sum(
            instruction_duration(self.instructions[index])
            for index in (14, 15, 16, 17)
        )
        recovery_sm_cycles = escape_setup_cycles + len(states) * raw_level_cycles
        recovery_transfer_clocks = (recovery_sm_cycles + 34 - 1) // 34
        idle_clocks = (5 * 400_000 + 1_000_000 - 1) // 1_000_000
        self.assertEqual((recovery_sm_cycles, recovery_transfer_clocks), (414, 13))
        self.assertEqual(recovery_transfer_clocks + idle_clocks, 15)

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
        )
        for contract in required_hil_contracts:
            with self.subTest(hil_contract=contract):
                self.assertIn(contract, self.hil_doc)

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
