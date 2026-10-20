# x1.2 Hardware And BSP

This document is the canonical BSP reference for the Radxa Linkr Debugger
`RA051_MB_X1.2` board. It records the facts the firmware Device Tree encodes
and the runtime consequences that are not obvious from the overlay alone.
Schematics are under `docs/hardware/`.

## SoC, Flash, And Boot

- MCU: **RP2354A** (QFN-60). RP2350A-compatible core and pinout.
- External QSPI `U23` (`W25Q64JV`) is **not populated**; the board uses the
  **2 MB internal flash**.
- The build keeps the `rpi_pico2/rp2350a/m33/mcuboot` board and the 4 MB
  sysbuild partition layout. All used regions fit below 2 MB: MCUboot at
  `0x0..0x10000`, slot0 `0x10000..0xE0000`, slot1 `0xE0000..0x1B0000`, and
  NVS storage starting at `0x1B0000` (8 sectors, 32 KB). The 4 MB layout only
  over-declares the storage partition; it does not move used data past 2 MB.
  Moving to `partitions_2M_sysbuild.dtsi` would require a repo-local MCUboot
  board because the upstream board DTS hardcodes the 4 MB include.

## Power Rails

All enables are active-high through a P-FET stage.

| Rail | GPIO | Net |
| --- | --- | --- |
| `5v_out` | GPIO0 | `GP0_5V_EN` (U16) |
| `vdd_5v` (aux 5 V) | GPIO1 | `GP01_5V_WS_EN` (U18) |
| `12v_out` | GPIO2 | `GP02_12V_EN` (U5) |
| `20v_out` | GPIO3 | `GP03_20V_EN` (U10) |

The x1.1 `reg_vio` / `reg_1v8_vio` regulators and the VIN 1.8 V/3.3 V route do
not exist on x1.2; board 3.3 V is generated from USB5V by U14. The firmware
persistent catalog therefore has **no `switch/vin` item**.

## Current And Voltage Sensing

Four **INA226** devices share one I2C bus. Each reports bus voltage, current,
and power; the rail telemetry reads both voltage and current from the same
device (there is no on-board ADC current path).

| Rail | Device | 7-bit address | Shunt |
| --- | --- | --- | --- |
| `20v_out` | U9 | `0x40` | 10 mOhm |
| `12v_out` | U15 | `0x41` | 10 mOhm |
| `5v_out` | U11 | `0x44` | 10 mOhm |
| `vdd_5v` | U12 | `0x45` | 10 mOhm |

The overlay uses `rshunt-micro-ohms = <10000>` and
`current-lsb-microamps = <250>` for all four devices.

## I2C Transport (PIO)

The schematic nets `RP2354_SCL`/`RP2354_SDA` are on **GPIO20/GPIO21**
respectively (SCL=GPIO20, SDA=GPIO21). This is the opposite of the RP2350
I2C0 pin functions and of the pico-examples `SCL = SDA + 1` layout, so the
board drives the bus with a **PIO I2C master** whose two wait instructions
are absolute `wait 1 gpio, 20` (SCL); SDA is sampled on GPIO21.

- Clock: **200 kHz** by default with **2.2 kΩ to 3.3 V**. The former 1 kHz
  bring-up setting exposed low-frequency timeout assumptions and is no longer
  used. A future faster setting must remain within the PIO program's measured
  timing limits; the current 32-cycle program is not Fast-mode compliant at a
  nominal 400 kHz because its ACK low phase is too short. At 200 kHz the
  shortest ACK low phase is approximately 2.34 us, above Fast-mode's 1.3 us
  minimum, and the conservative bus-clear train runs at approximately 160 kHz.
  For faster operation,
  choose the pull-up from measured bus capacitance
  and verify `tr <= 300 ns`: external 2.2/1.5/1.2 kΩ support approximately
  161/236/295 pF respectively before accounting for CH224A's internal
  7–15 kΩ pull-up. Do not make 1.2 kΩ the default: its worst-case sink current
  approaches the 3 mA I2C VOL guarantee. Above roughly 250–300 pF, reduce
  speed or isolate/buffer the bus instead of further lowering resistance.
- CH224A is on the same bus and is limited to 400 kHz, so the shared bus
  cannot use INA226 High-Speed mode (2.94 MHz) without a hardware
  bridge/isolator. The PIO master also implements Fast mode only.
- CH224A VHV (PD VBUS) and INA226 VS (VCC_3V3) are separate power domains.
  CH224A specifies no powered-off Ioff, and INA226 SCL has an absolute maximum
  of `VS + 0.3 V`. Hardware validation must therefore cover both one-sided
  power sequences; if VHV can be present while INA226 VS is absent, use a
  powered-off-safe bus switch/isolator or enforce rail sequencing. Pull-up
  resistance alone cannot remove this back-power risk.
- CH224A CFG1 uses board R81 (120 kOhm to GND) as the single-resistor Rset,
  which enables its I2C interface on CFG2/SCL and CFG3/SDA. R97 is only the
  0-ohm series link from CFG1 to GPIO19; firmware keeps GPIO19 as a
  high-impedance input without pulls so it cannot override R81 and enter
  I/O-level mode.
- Although CH224A itself supports a 28 V fixed request, the x1.2 power path is
  limited to **20 V**. The CH224A Device Tree node sets
  `maximum-request-millivolt = <20000>`, which caps fixed and PPS requests.
- Register `0x09` may validly read zero when no charging protocol is active;
  presence is determined by I2C transaction success, not a nonzero status.
  Register `0x0A` and the PPS/AVS control registers are write-only. Register
  `0x50` is read only while a PD contract is active.
- Driver: `apps/radxa_linkr_debugger/src/i2c_rpi_pico_pio.c`, compatible
  `raspberrypi,pico-pio-i2c`, based on the pico-examples `pio/i2c` program.
  An initial START uses the official three-instruction sequence
  `SC1_SD0/SC0_SD0/mov isr`; repeated START uses the five-instruction
  `SC0_SD1/SC1_SD1/SC1_SD0/SC0_SD0/mov isr` sequence.
- The controller remains registered in a ready-but-paused state when startup
  finds a non-idle electrical bus, so `i2c recover i2c` remains available.
  Startup passively settles the lines and performs at most one automatic,
  open-drain bus clear only for stable SCL-high/SDA-low. If INA226 device init
  already failed before a later explicit recovery, run a software reboot after
  recovery; Zephyr does not re-run a failed sensor device initializer in place.
- GP20 is the I2C **SCL** and also the WIDE11 logic-analyzer lane 10. The LA
  leaves the PIO0 function and pad input enable untouched for GP20/GP21 and
  samples GP20 only as a read-only tap; it must not reconfigure those pins as
  SIO/PIO2. GP21 is SDA. INA226 telemetry while WIDE11 is running is still a
  HIL item.
- The DHCP captive-portal URI remains the compile-time
  `http://172.29.203.1/...`; for a non-default subnet open
  `http://172.29.<subnet>.1/` directly.

## J4 Target UART

J4 is a 3-pin (TX/RX/GND) target UART on **UART1** (TX=GPIO10, RX=GPIO11,
default `115200`). The production firmware bridges UART1 to a second USB CDC
ACM (`cdc_acm_uart1`) with the `zephyr,uart-bridge` driver. The host sees a
dedicated J4 tty in addition to the console CDC ACM (`cdc_acm_uart0`) and the
NCM interface. The x1.2 CH347F is not used for this path; the x1.1 CH347 is a
separate USB device on the older board.

- The overlay enables `&uart1` on GPIO10/11. The upstream `UART1_TX_P10`/
  `UART1_RX_P11` helpers reference an undefined
  `RP2_PINCTRL_GPIO_FUNC_UART_ALT` in this Zephyr tree, so the overlay uses
  `RP2XXX_PINMUX(..., RP2_PINCTRL_GPIO_FUNC_UART_AUX)` (RP2350 function 11)
  directly.
- `j4_uart_bridge` has `peers = <&cdc_acm_uart1 &uart1>`; the USB message
  callback applies CDC ACM line coding to the bridge.

## Header GPIO And Board Controls

- `ADC0`-`ADC3` are GPIO26-GPIO29 and are exposed on J8; they are not rail
  sense inputs.
- Safe-GPIO catalog: `GP7`-`GP9` (J13), `GP12`-`GP18`, and input-only `GP29`
  (J8). `GP20`/`GP21` are **not** safe GPIOs: they are the PIO I2C SCL/SDA
  bus brought out on J8 for external I2C devices, and the firmware does not
  expose them as GPIO controls. `GP10`/`GP11` belong to the J4 UART and
  `GP19` is the CH224A PD strap, so they are reserved.
- `LED_BLUE` = GPIO24, `GP06_TF_SW` = GPIO4 (TF/SD mux), `TF_WP` = GPIO22,
  `OTG_USB_EN` = GPIO23, `RP2354_PD_CFG1` = GPIO19.

### BOOTSEL Button And Restart

The physical BOOTSEL button is sampled on the QSPI flash chip-select pin with
the QSPI output driver temporarily disabled. The Device Tree node
`bootsel-button` sets a 20 ms sample interval, 3-sample debounce, and a
3000 ms long-press threshold.

- A short press does nothing.
- Holding the button for 3 seconds while firmware runs arms a restart on
  release. Releasing it performs a **normal cold restart**, not ROM BOOTSEL.
- Holding the button without releasing never resets the board.
- Holding the button during power-on or any reset is sampled by the RP2350
  ROM, which enters ROM BOOTSEL instead of starting firmware.

The button is polled by a firmware thread, not handled in an ISR or by the
hardware watchdog. It therefore cannot run when the core is completely wedged;
in that failure mode the hardware watchdog remains the fallback. A normal
watchdog reset enters ROM BOOTSEL through the recovery marker. When the OTA
test marker is present, the same reset is handled by MCUboot rollback instead,
as described in the OTA contract.

A normal software restart (`reboot` / `POST /api/v1/reboot`) is a cold restart
that never calls the ROM USB boot entry. An explicit ROM BOOTSEL request
(`bootloader` / `POST /api/v1/bootloader`) calls `reset_usb_boot()` and is not
an alias for reboot. A rail not restored by a saved v1 configuration snapshot
returns to its `regulator-boot-off` default on every cold restart; `vdd_5v`
follows the `switch usb` boot route.

## Validation Status

The BSP has been validated by the full firmware build and the host-model unit
tests. Earlier on-hardware x1.2 bring-up at 1 kHz verified the four INA226
addresses (`0x40/0x41/0x44/0x45`), manufacturer ID `0x5449`, configuration
register `0x4127`, and idle SDA/SCL levels. CH224A (`0x22/0x23`) I2C,
100/400 kHz operation with external pull-ups, rail current calibration,
rail/LED polarity, the J4 UART bridge, INA226 telemetry while WIDE11
captures the GP20/SCL tap, and the new reboot and physical BOOTSEL button
paths remain HIL items.
