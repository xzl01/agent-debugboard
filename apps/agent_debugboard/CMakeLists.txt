# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(agent_debugboard)

zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section(NAME http_resource_desc_debugboard_http_service
  KVMA RAM_REGION GROUP RODATA_REGION)

target_sources(app PRIVATE
  src/main.c
  src/debugboard_control.c
  src/debugboard_http.c
  src/debugboard_monitoring.c
  src/debugboard_model.c
  src/debugboard_network.c
  src/debugboard_shell.c
  src/debugboard_ws.c
  src/debugboard_usb_net.c
)
