# SPDX-License-Identifier: Apache-2.0
#
# Raspberry Pi Pico 2 test firmware: 2x UART->USB CDC ACM + CMSIS-DAP SWD.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(pico2_uart_swd_test)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

target_sources(app PRIVATE
  src/main.c
)
