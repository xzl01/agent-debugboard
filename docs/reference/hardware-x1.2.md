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

All enables are active-high through a P-FET or load-switch stage.

| Rail | GPIO | Net | Notes |
| --- | --- | --- | --- |
| `5v_out` | GPIO0 | `GP0_5V_EN` (U16) | independent target rail; `regulator-boot-off` |
| `vdd_5v` (aux 5 V) | GPIO1 | `GP01_5V_WS_EN` (U18) | parent auxiliary rail; no boot-on property |
| `12v_out` | GPIO2 | `GP02_12V_EN` (U5) | independent target rail; `regulator-boot-off` |
| `20v_out` | GPIO3 | `GP03_20V_EN` (U10) | independent target rail; `regulator-boot-off` |
| `5v_usb_hub` | GPIO24 | `GPIO24_USB_CRL` (U38) | child of `vdd_5v`; `regulator-boot-off` |

`5v_usb_hub` is the switched USB-hub 5 V branch. Firmware now exposes it as a
fifth controllable power output alongside the three target rails and
`vdd_5v`. Its setter is parent-aware: enabling it enables `vdd_5v` first when
needed; disabling it leaves `vdd_5v` unchanged; disabling `vdd_5v` disables
`5v_usb_hub` first; enabling `vdd_5v` alone does not enable the hub branch.
The firmware rail model owns that parent relationship; Device Tree owns each
regulator's boot-off state.

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

`5v_usb_hub` has no INA226 and no dedicated ADC channel. The `vdd_5v` INA226
reports the parent-rail total, which includes the hub branch; it is not a
separate hub-current measurement. The ADC telemetry contract therefore remains
eight descriptors even though the power-output catalog has five entries.

## I2C Transport (PIO)

The schematic nets `RP2354_SCL`/`RP2354_SDA` are on **GPIO20/GPIO21**
respectively (SCL=GPIO20, SDA=GPIO21). This is the opposite of the RP2350
I2C0 pin functions and of the pico-examples `SCL = SDA + 1` layout, so the
board drives the bus with a **PIO I2C master**. Its data/ACK waits and all
escape-path waits are encoded at initialization from the Device Tree SCL GPIO;
there is no machine-code dependency on GPIO20. SDA is sampled through the
Device Tree-selected input pin.

- Clock: **400 kHz production default**. The PIO data-bit loop is 34
  state-machine cycles: 18 low and 16 high. At the RP2350 150 MHz system clock,
  divider `11 + 8/256` gives approximately **399.933 kHz**, while the shortest
  data/ACK low phase remains 18 cycles (approximately 1.324 us).
- The production BOM formally populates **4.7 kΩ SCL and SDA pull-ups to the
  always-powered board VCC_3V3 rail**. Firmware retains `bias-disable`; RP2350
  internal pulls are not part of the product pull-up network. The bus also
  joins RP2354 GPIO20/21, CH224A, four VCC_3V3-powered INA226 devices, and J8.
  The qualified 400 kHz baseline requires J8 to be unloaded.
  - CH224A documents selectable **7/10/15 kΩ internal pull-ups** on CFG2/CFG3
    and support for 400 kHz, but does not identify the pull-up rail or specify
    powered-off Ioff when VHV is 0 V. CFG pins have a 6.5 V absolute maximum.
    Those limits do not turn the internal pulls into a qualified cross-domain
    pull-up source.
  - INA226 SCL has an absolute maximum of `VS + 0.3 V`; with `VS = 0 V`, a
    3.3 V-high SCL is explicitly outside that limit. SDA has a 6 V absolute
    maximum, but INA226 specifies no powered-off Ioff, so its lower clamp limit
    alone does not establish reverse-current safety.
  - RP2350 internal RPU is **32–86 kΩ**. It is not the specified 4.7 kΩ BOM
    pull-up and would create an uncontrolled additional VCC_3V3 path. Firmware
    must never enable it as a workaround: initialization
    disables both internal pulls before pinctrl removes pad isolation, and the
    Device Tree keeps `bias-disable` on both pins.
  - The board's ESD5451N is a **5 V TVS**; it does not clamp the shared bus at
    3.3 V and is not powered-off isolation or back-feed protection.
- The 400 kHz product contract does not authorize arbitrary J8 loading. Any
  external device, cable, pull-up, or other power domain must be treated as a
  new bus configuration: prohibit arbitrary or cross-domain pull-ups and
  re-calculate and measure total `Cb`, 30%–70% `tr`, `VOL`/sink current, and
  powered/unpowered current paths before claiming 400 kHz support.
- Data and ACK high phases wait for physical SCL. After WAIT observes SCL high,
  each path has an explicit nine-state-machine-cycle high hold: at every phase
  of the `11 + 8/256` divider this is at least 660 ns. STOP and repeated START
  likewise execute a dynamic SCL WAIT immediately after release, then hold the
  actual-high state for ten cycles (at least 733 ns) before changing SDA.
  Clock stretching therefore only lengthens these phases.
- Recovery applies the same dynamic WAIT to every one of its nine pulse highs
  and to the tail STOP. A no-stretch recovery pulse is 43 state-machine cycles,
  approximately **316.226 kHz** at the 400 kHz production setting. The STOP
  tail holds SCL low for 20 cycles,
  waits for physical SCL high, then holds SDA low for ten cycles before release.
  The deadline accounts for 41 raw levels, ten WAIT instructions, parser/EXEC
  overhead, and tBUF. A stretched-low SCL eventually expires the CPU-side
  deadline and fail-closes with both lines released; it cannot leave an
  unbounded CPU polling loop or a partially queued escape active.
- Device Tree enables RP2350 input Schmitt hysteresis on both SDA and SCL while
  retaining `bias-disable` and open-drain operation. Hysteresis improves input
  edge qualification, but it does **not** by itself prove the UM10204 50 ns
  spike-suppression requirement. These software timing checks are not a claim
  of complete UM10204 electrical certification. The former 1 kHz bring-up
  setting exposed low-frequency timeout assumptions and is no longer used.
- A target NAK enters the PIO IRQ wait path. Before issuing cleanup STOP, the
  driver forces physical OE off, disables the SM, clears both FIFOs, restarts
  the shift counters/ISR/EXEC/delay/IRQ-wait state, explicitly consumes the
  preserved OSR contents with autopull disabled, restores the parser PC and
  original shift configuration (including read autopush), then safely re-enables
  open drain. This prevents a prefetched WAIT or partial escape word from being
  decoded as the new STOP header. Successful cleanup still returns the original
  NAK `-EIO`; a cleanup timeout/error remains the reported diagnostic instead.
- **Qualified baseline calculation:** the production 4.7 kΩ pull-ups are tied
  to always-powered board VCC_3V3 and J8 is unloaded. The characterization HIL
  used electrically equivalent 4.7 kΩ fly-wire pull-ups, with VHV/PD online and
  no one-sided power-off case. In parallel with the documented
  CH224A 7/10/15 kΩ internal pull-ups, the calculated effective values are
  2.812/3.197/3.579 kΩ. With the known 34 pF fixture capacitance this estimates
  `tr = 81.0–103.1 ns` and a 3.3 V low-state pull-up current of
  0.922–1.174 mA. The 4.7 kΩ external pull-up alone gives `tr = 135.4 ns` and
  `Cbmax = 75.33 pF` at the 300 ns Fast-mode limit. The worst-case parallel
  value gives `Cbmax = 98.94 pF`, leaving 64.94 pF beyond the known 34 pF.
  These calculations define the unloaded baseline; they do not authorize extra
  J8 capacitance/pull-ups or prove one-sided powered-off safety.
- CH224A is on the same bus and is limited to 400 kHz, so the shared bus
  cannot use INA226 High-Speed mode (2.94 MHz) without a hardware
  bridge/isolator. The PIO master also implements Fast mode only.
- CH224A VHV (PD VBUS) and INA226 VS (VCC_3V3) are separate power domains.
  ADC0 observes the `DC20V` source after Q28 and before U10 through R46/R47
  (100 kΩ upper, 15 kΩ lower; 115 kΩ full path). This source-side divider is
  not a direct VHV measurement. A private standard `voltage-divider` sensor
  owns these Device Tree facts and is used only under the CH224A driver lock;
  the RP2350 ADC driver's `adc_context` serializes it with other ADC channels.
  It is not added as a public telemetry descriptor.
- Every real CH224A I2C read or write is preceded by a fresh source-voltage
  sample, including each probe address/attempt, each I2C retry, every one of the
  48 capability bytes, and both PPS writes. From unknown/absent state, three
  consecutive samples of at least 2.8 V at 5 ms intervals establish presence
  (at least 10 ms total). Once present, each transaction may continue at or
  above 2.5 V; the 2.5–2.8 V band retains only an already-present state. A
  value below 2.5 V, sensor-not-ready state, ADC fetch/get error, or conversion
  error immediately clears input/device presence and prevents any remaining
  retry or subsequent write. Status and capability reads perform one additional
  fresh endpoint check after their final I2C byte.
- The CH224A Device Tree node uses standard `zephyr,deferred-init` so its
  priority-51 init entry is skipped during automatic boot initialization.
  `linkr_debugger_pd_init()` calls `device_init()` from the application main
  path, after the I2C controller and voltage-divider sensor are initialized;
  this explicit initializer remains bus-silent. It then starts one dedicated
  priority-12, 1024-byte-stack monitor thread. About every 100 ms the monitor
  takes only a private gate sample while input remains present; it does not use
  the system workqueue and does not repeatedly read status or the 48-byte
  capability window. A stable absent-to-present edge triggers gated
  probe/current-status discovery and any initial capability read required by
  the existing cache rules. A fresh source edge clears only the old probe
  backoff, so recovery does not depend on an HTTP/WS/client status request and
  never restores an old voltage request. This is periodic ADC polling, not an
  IRQ or comparator interrupt. A gate/ADC/I2C failure clears requested voltage,
  gear/PPS state, and
  capability data, then starts the existing five-second probe backoff; source
  insertion can recover only through a fresh stable probe and does not revive
  an old voltage request. A complete capability window containing no valid PDO
  uses only capability backoff, and short status never publishes an expired
  five-second capability cache. The 5 V fallback skips capability parsing but
  never bypasses the voltage gate.
- The gate prevents the **next** transaction after a low observation. It cannot
  eliminate the ADC-sample-to-I2C-start TOCTOU window or cancel an in-flight
  transaction. It also cannot remove DC20V residual charge/backfeed, static
  paths through the external 4.7 kΩ pull-ups or an unpowered device, or clocks
  shared with the INA226 devices. CH224A still has no vendor-guaranteed
  powered-off Ioff; polling or successful ADC gating does not prove Ioff safety,
  zero bus waveforms while VHV is absent, or isolation. These are
  non-frequency-specific hardware boundaries that 400 kHz timing and the 5 V
  TVS cannot resolve.
- CH224A CFG1 uses board R81 (120 kOhm to GND) as the single-resistor Rset,
  which enables its I2C interface on CFG2/SCL and CFG3/SDA. R97 is only the
  0-ohm series link from CFG1 to GPIO19; firmware keeps GPIO19 as a
  high-impedance input without pulls so it cannot override R81 and enter
  I/O-level mode.
- Although CH224A itself supports a 28 V fixed request, the x1.2 power path is
  limited to **20 V**. The CH224A Device Tree node sets
  `maximum-request-millivolt = <20000>`, which caps fixed and PPS requests.
- Register `0x09` presence/read validity is defined solely by a successful I2C
  ACK and one-byte read, not by the returned value. Firmware decodes only its
  named bit masks and ignores unknown or reserved bits for forward
  compatibility; values including `0x00`, `0x80`, `0x88`, and `0xff` are not
  rejected. The first status byte per boot containing unknown bits emits one
  warning with the raw byte, without log flooding, retries, bus recovery, or PD
  state loss. A divider-reconstructed source candidate around 3.3 V (about
  0.43 V at the ADC pad) therefore permits a probe but is not sufficient by
  itself: an acknowledged one-byte CH224A status read is still required before
  the device is available.
  Register `0x0A` and the PPS/AVS control registers are write-only. Register
  `0x50` is read only while a PD contract is active.
- Driver: `apps/radxa_linkr_debugger/src/i2c_rpi_pico_pio.c`, compatible
  `raspberrypi,pico-pio-i2c`, based on the pico-examples `pio/i2c` program.
  An initial START uses the official three-instruction sequence
  `SC1_SD0/SC0_SD0/mov isr`; repeated START uses the five-instruction
  `SC0_SD1/SC1_SD1/SC1_SD0/SC0_SD0/mov isr` sequence.
- The controller remains registered in a ready-but-paused state when startup
  finds a non-idle electrical bus. Startup passively settles the lines and
  performs at most one automatic, open-drain bus clear only for stable
  SCL-high/SDA-low; a later transfer can rearm only after both lines are stably
  idle. The driver's standard `.recover_bus` callback already runs the same
  bounded, bus-locked, fail-closed sequence: output latches stay zero, physical
  OE is released before reconfiguration, SCL is pulled low/released only through
  PINDIRS, and no internal pull or push-pull HIGH is introduced. A CH224A probe
  may invoke it once only for `-ETIMEDOUT` or `-EBUSY`, with a fresh ADC gate
  before recovery and again before the retry. Clean address/data NAK (`-EIO`)
  never invokes recovery, preventing a bus-clear storm across address retries.
  Recovery failure leaves CH224A unavailable under the five-second probe
  backoff. The generic Zephyr I2C shell remains disabled because its raw commands
  can bypass CH224A power policy; no arbitrary-address/configuration interface
  is added. After correcting an INA226 initialization fault, reboot is still
  required because Zephyr does not re-run that sensor initializer in place.
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
default `115200`). The runtime accepts every integer baud in the inclusive range
4800 through 115200; the five recommended UI presets are 9600, 19200, 38400,
57600, and 115200. Higher rates such as 230400, 921600, and 1500000 are rejected
and are not product paths; earlier high-rate samples are diagnostic evidence
only.
Production does **not** use Zephyr's generic UART bridge.
The repo-local `linkr_debugger_serial` runtime owns the physical UART, line
coding, bounded TX queue, completion events, and the shared 4096-byte RX ring.
It fans RX independently to the J4 CDC ACM interface, four WebSocket sinks, and
two raw-TCP sinks; raw TCP listens on `0.0.0.0:5555`.

- The overlay enables `&uart1` on GPIO10/11. The upstream `UART1_TX_P10`/
  `UART1_RX_P11` helpers reference an undefined
  `RP2_PINCTRL_GPIO_FUNC_UART_ALT` in this Zephyr tree, so the overlay uses
  `RP2XXX_PINMUX(..., RP2_PINCTRL_GPIO_FUNC_UART_AUX)` (RP2350 function 11)
  directly.
- `/chosen` binds `radxa,linkr-j4-uart = &uart1` and
  `radxa,linkr-j4-cdc = &cdc_acm_uart1`. Application code resolves those as
  `DT_CHOSEN(radxa_linkr_j4_uart)` and `DT_CHOSEN(radxa_linkr_j4_cdc)`; no
  board pin map or peer list is duplicated in host tools.
- USB CDC line-coding, DTR/RTS, reset, and configuration notifications enter
  the same serial runtime used by HTTP/WS/TCP. The final x1.2 HIL enumerated the
  control shell as ACM3 and J4 as ACM4, but host tty numbering is not a stable
  API.
- CDC line control is an internal four-state machine (DOWN/PROMOTING/UP/BLOCKED).
  A USB default line-coding notification while DTR is false only stages a
  candidate tuple; it does not change active line coding or generation. HTTP/WS
  config in that low state applies immediately. LOW→HIGH applies any staged
  tuple and only then publishes DTR/ingress; a failed apply/attach/detach stays
  blocked until a successful LOW/reset. Notification sequence/inflight and
  session-epoch/mailbox identity fence promotion. Production requires deferred
  USB notifications: callbacks stage state and wake the control worker rather
  than performing HAL work in notification context. The external API does not
  expose the internal phase or block reason.
- The embedded xterm.js terminal subscribes to live-session topic `serial.j4`.
  The current product J4 path never uses the still-shipped legacy host
  `uart0`/`uart1` broker or browser Web Serial.
- Explicit J4 input remains multiwriter and message-atomic; there is no TX-owner
  acquisition and non-responding clients are not read-only. Exactly one client
  is selected as the automatic terminal responder: CDC while DTR is asserted,
  otherwise the lowest-numbered attached WebSocket slot. That client alone
  answers terminal DSR/DA/CPR queries. Other Web clients still send explicit
  keyboard, IME, and paste input, escape sequences are not filtered, and a
  rejected write does not lock out later input.
- State and data epoch boundaries, a renderer rebuild on generation changes, and
  a responder-promotion queue with a zero-byte parser fence prevent a passive
  terminal's stale partial or queued query from being answered after promotion.
  The promotion path keeps the same xterm modes. See
  [`serial-broker.md`](serial-broker.md) for the wire fields and the finite
  responder matrix evidence.

## Header GPIO And Board Controls

- J8 and J13 are both on the board's **right edge**. All positions below use a
  component-side top view; row/column coordinates must not be mirrored merely
  because the connector is on the right edge.
- J8 is 2x10: from the top, the left column is pins `10` through `1`, and the
  right column is pins `20` through `11`. The confirmed relevant mapping is:

  | J8 pin | Signal | Ownership |
  | --- | --- | --- |
  | 10 | GP21 | PIO I2C SDA; not safe GPIO |
  | 20 | `5V_USB_HUB` | switched `5v_usb_hub` child-rail output; not a GPIO/ADC |
  | 9 | GP20 | PIO I2C SCL; not safe GPIO |
  | 8 | GP27 / ADC1 | header ADC; not rail sensing |
  | 7 | GP28 / ADC2 | header ADC; not rail sensing |
  | 6 | GP29 / ADC3 | `adc3`; safe catalog input-only |
  | 5 | GP16 | safe GPIO |
  | 4 | GP15 | safe GPIO |
  | 3 | GP14 | safe GPIO |
  | 2 | GP13 | safe GPIO |
  | 1 | GP12 | safe GPIO |
  | 16 | GP5 / `GPIO5_CON` | direct safe GPIO; default input, no pulls |
  | 15 | GP6 / `GPIO6_CON` | direct safe GPIO; default input, no pulls |

  ADC0/GP26 has no J8 terminal in this confirmed mapping. This table is a
  confirmed subset, not a complete J8 pinout; unlisted pins 11-19 are not
  inferred here.
- J13 is 2x2 in the same view: top-left pin 2 is GP9/USER, top-right pin 3 is
  GP8/RSET, bottom-left pin 1 is GP7/MASKROM, and bottom-right pin 4 is **GND**,
  not 3.3 V.
- The safe-GPIO allowlist is output-capable `GP5`, `GP6`, `GP7`-`GP9`,
  `GP12`-`GP16`, and input-only `GP29`. GP17 and GP18 are absent. Across the 24
  J8/J13 physical cells, 11 are controls and 13 are inert; 10 controls are
  output-capable. `GP10`/`GP11`
  belong to J4 UART and `GP19` is the CH224A PD strap, so they are reserved.
- The x1.2 switch catalog contains only `sd` and `tf_wp`. `switch/sd` uses
  GPIO4 / `GP06_TF_SW`; `switch/tf_wp` uses GPIO22 / `TF_WP`. There is no
  `switch/usb`; `/api/v1/switch/usb` returns 404. `vdd_5v` is independent of
  switch routing, and `5v_usb_hub` is its power child.
- `LED_BLUE` = GPIO25 (active-high status LED), `5v_usb_hub` enable =
  `GPIO24_USB_CRL` = GPIO24, `GP06_TF_SW` = GPIO4 (TF/SD mux), `TF_WP` =
  GPIO22, `OTG_USB_EN` = GPIO23, `RP2354_PD_CFG1` = GPIO19.

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
returns to its inactive Device Tree boot state on every cold restart;
`5v_usb_hub` also boots off and remains off unless its own snapshot entry or an
explicit setter enables it. `vdd_5v` is the parent output, not a route side
effect, and enabling it does not implicitly enable the hub branch.

## Validation Status

The BSP has been validated by the full firmware build and the host-model unit
tests. Static policy tests cover the 400 kHz production default, 150 MHz
divider, generated PIO machine code, Fast-mode timing
bounds, all 32 fractional-divider phases, dynamic SCL stretching, escape/FIFO
timeout fail-close behavior, and recovery rate. Earlier on-hardware
x1.2 bring-up at 1 kHz verified the four INA226
addresses (`0x40/0x41/0x44/0x45`), manufacturer ID `0x5449`, configuration
register `0x4127`, and line levels in that specific setup; those observed levels
do not identify a pull-up source or prove cross-domain safety. CH224A
(`0x22/0x23`) I2C, any 400 kHz enablement with explicit external pull-ups and
the required rise-time/VOL/reverse-current evidence, rail current calibration,
rail/LED polarity, INA226 telemetry while WIDE11 captures the GP20/SCL tap,
and the physical BOOTSEL button path remain HIL items. Current J4
CDC/WS/raw-TCP fan-out, the 4800..115200 product ceiling and high-rate
diagnostics, CDC line-coding staging/promotion, the fifth hub power output, the
J4 automatic-responder matrix, and post-capture restoration evidence are in
[the 2026-10-21 x1.2 report](../testing/results/2026-10-21-x1.2-adc-pinout-j4-hil.md);
its stated limitations remain authoritative.
