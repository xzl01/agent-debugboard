# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
# Copyright (c) Jiali Chen <chenjiali@radxa.com>

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radxa_linkr_debugger)

zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section(NAME http_resource_desc_linkr_debugger_http_service
  KVMA RAM_REGION GROUP RODATA_REGION)

target_sources(app PRIVATE
  src/main.c
  src/linkr_debugger_control.c
  src/linkr_debugger_http.c
  src/linkr_debugger_monitoring.c
  src/linkr_debugger_model.c
  src/linkr_debugger_network.c
  src/linkr_debugger_shell.c
  src/linkr_debugger_ws.c
  src/linkr_debugger_usb_net.c
)
