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

find_program(NPM_EXECUTABLE NAMES npm REQUIRED)

set(linkr_debugger_web_dir ${CMAKE_CURRENT_SOURCE_DIR}/../../web)
set(linkr_debugger_web_dist_dir ${ZEPHYR_BINARY_DIR}/web-dist)
set(linkr_debugger_web_gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)
set(linkr_debugger_web_dist_files
  ${linkr_debugger_web_dist_dir}/index.html
  ${linkr_debugger_web_dist_dir}/assets/app.css
  ${linkr_debugger_web_dist_dir}/assets/app.js
)

file(GLOB_RECURSE linkr_debugger_web_sources CONFIGURE_DEPENDS
  ${linkr_debugger_web_dir}/src/*
  ${linkr_debugger_web_dir}/scripts/*
)
list(APPEND linkr_debugger_web_sources
  ${linkr_debugger_web_dir}/index.html
  ${linkr_debugger_web_dir}/package-lock.json
  ${linkr_debugger_web_dir}/package.json
  ${linkr_debugger_web_dir}/postcss.config.js
  ${linkr_debugger_web_dir}/tailwind.config.js
  ${linkr_debugger_web_dir}/tsconfig.json
  ${linkr_debugger_web_dir}/tsconfig.node.json
  ${linkr_debugger_web_dir}/vite.config.ts
)

add_custom_command(
  OUTPUT ${linkr_debugger_web_dist_files}
  COMMAND ${NPM_EXECUTABLE} ci --ignore-scripts --no-audit --no-fund
  COMMAND ${CMAKE_COMMAND} -E env VITE_OUT_DIR=${linkr_debugger_web_dist_dir}
          ${NPM_EXECUTABLE} run build:firmware
  DEPENDS ${linkr_debugger_web_sources}
  WORKING_DIRECTORY ${linkr_debugger_web_dir}
  COMMENT "Building embedded Web UI"
  VERBATIM
)

generate_inc_file_for_target(app
  ${linkr_debugger_web_dist_dir}/index.html
  ${linkr_debugger_web_gen_dir}/linkr_debugger_web_index.html.gz.inc
  --gzip
)
generate_inc_file_for_target(app
  ${linkr_debugger_web_dist_dir}/assets/app.css
  ${linkr_debugger_web_gen_dir}/linkr_debugger_web_app.css.gz.inc
  --gzip
)
generate_inc_file_for_target(app
  ${linkr_debugger_web_dist_dir}/assets/app.js
  ${linkr_debugger_web_gen_dir}/linkr_debugger_web_app.js.gz.inc
  --gzip
)

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
