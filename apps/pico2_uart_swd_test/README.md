<!--
SPDX-License-Identifier: Apache-2.0
-->

# Pico 2 2x UART + SWD test build

This is a separate test firmware for a bare Raspberry Pi Pico 2 (RP2350A,
no MCUboot). It combines:

- two hardware UARTs bridged to two USB CDC ACM interfaces;
- a CMSIS-DAP v2 SWD probe over USB bulk.

It does not use the Radxa Linkr production firmware or the onboard CH347F.

## Build

```sh
make pico2-uart-swd-test
```

The output is `build/pico2_uart_swd_test/zephyr/zephyr.uf2`. Copy that file
to the Pico 2 BOOTSEL mass-storage device, for example:

```sh
picotool load -v -x build/pico2_uart_swd_test/zephyr/zephyr.uf2
```

## Wiring

| Pico 2 pin | Function | Target |
|---|---|---|
| GP0 | UART0 TX | target RX |
| GP1 | UART0 RX | target TX |
| GP4 | UART1 TX | target RX |
| GP5 | UART1 RX | target TX |
| GP12 | SWD CLK | target SWCLK |
| GP14 | SWD DIO | target SWDIO |
| GND | ground | target GND |

Both UART channels expect 3.3 V logic. Use a level shifter for a 5 V target.

## Host verification

```sh
# enumerate the two CDC ACM UART bridge ports
ls -l /dev/ttyACM* /dev/ttyUSB*

# verify the CMSIS-DAP probe is visible
pyocd list
```

From the host, each bridge port behaves like a normal serial port. The CMSIS-DAP
interface can be used by pyOCD, OpenOCD (`interface/cmsis-dap.cfg`), or another
CMSIS-DAP v2 client.
