# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
#
# Post-sysbuild combined UF2 generation. The app CMakeLists.txt regenerates
# the app-only UF2 from the signed hex during its own build step, but the
# combined MCUboot+app UF2 must wait until both images are fully built.
#
# File-level dependencies are resolved by CMake at generate time (after all
# images are registered), so mcuboot/zephyr/zephyr.hex is correctly resolved
# even though the MCUboot ExternalProject is not yet added when this file is
# included.
set(combined_out "${CMAKE_BINARY_DIR}/radxa-linkr-debugger-rp2350.uf2")
set(app_hex     "${CMAKE_BINARY_DIR}/radxa_linkr_debugger/zephyr/zephyr.signed.hex")
set(mcuboot_hex "${CMAKE_BINARY_DIR}/mcuboot/zephyr/zephyr.hex")

add_custom_command(
  OUTPUT ${combined_out}
  COMMAND ${CMAKE_COMMAND}
    -DAPP_BUILD_DIR=${CMAKE_BINARY_DIR}/radxa_linkr_debugger
    -DSYSBUILD_DIR=${CMAKE_BINARY_DIR}
    -DZEPHYR_BASE=${ZEPHYR_BASE}
    -DPYTHON=${Python3_EXECUTABLE}
    -P ${SOURCE_DIR}/cmake/merge_uf2.cmake
  DEPENDS ${app_hex} ${mcuboot_hex}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Generating combined MCUboot+app UF2"
)

add_custom_target(linkr_debugger_combined_uf2 ALL
  DEPENDS ${combined_out}
)

