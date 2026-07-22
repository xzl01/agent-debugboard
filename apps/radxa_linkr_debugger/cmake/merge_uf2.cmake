# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
#
# Post-build UF2 artifact fixup for MCUboot builds. Zephyr emits zephyr.uf2
# from the raw (header-less) application hex, which MCUboot refuses to boot;
# BOOTSEL-flashing that file bricks the board until manual recovery. This
# script regenerates the app-only UF2 from the imgtool-formatted signed hex
# (unsigned payload, but valid MCUboot header) and merges MCUboot + app into
# the canonical combined UF2 referenced by the documentation.

if(NOT DEFINED APP_BUILD_DIR OR NOT DEFINED SYSBUILD_DIR OR NOT DEFINED ZEPHYR_BASE)
	message(FATAL_ERROR "APP_BUILD_DIR, SYSBUILD_DIR and ZEPHYR_BASE must be set")
endif()

if(NOT DEFINED PYTHON OR PYTHON STREQUAL "")
	set(PYTHON python3)
endif()

set(MCUBOOT_HEX "${SYSBUILD_DIR}/mcuboot/zephyr/zephyr.hex")
set(APP_SIGNED_HEX "${APP_BUILD_DIR}/zephyr/zephyr.signed.hex")
set(APP_UF2 "${APP_BUILD_DIR}/zephyr/zephyr.uf2")
set(COMBINED_HEX "${SYSBUILD_DIR}/radxa-linkr-debugger-rp2350-combined.hex")
set(COMBINED_UF2 "${SYSBUILD_DIR}/radxa-linkr-debugger-rp2350.uf2")

if(NOT EXISTS "${APP_SIGNED_HEX}")
	message(FATAL_ERROR "signed application hex missing: ${APP_SIGNED_HEX}")
endif()

execute_process(
	COMMAND "${PYTHON}" "${ZEPHYR_BASE}/scripts/build/uf2conv.py"
		-c -f 0xe48bff57 -o "${APP_UF2}" "${APP_SIGNED_HEX}"
	RESULT_VARIABLE uf2_rc)
if(NOT uf2_rc EQUAL 0)
	message(FATAL_ERROR "uf2conv failed for app-only UF2: ${uf2_rc}")
endif()

if(EXISTS "${MCUBOOT_HEX}")
	execute_process(
		COMMAND "${PYTHON}" "${ZEPHYR_BASE}/scripts/build/mergehex.py"
			-o "${COMBINED_HEX}" "${MCUBOOT_HEX}" "${APP_SIGNED_HEX}"
		RESULT_VARIABLE merge_rc)
	if(NOT merge_rc EQUAL 0)
		message(FATAL_ERROR "mergehex failed: ${merge_rc}")
	endif()
	execute_process(
		COMMAND "${PYTHON}" "${ZEPHYR_BASE}/scripts/build/uf2conv.py"
			-c -f 0xe48bff57 -o "${COMBINED_UF2}" "${COMBINED_HEX}"
		RESULT_VARIABLE combined_rc)
	if(NOT combined_rc EQUAL 0)
		message(FATAL_ERROR "uf2conv failed for combined UF2: ${combined_rc}")
	endif()
	message(STATUS "Combined MCUboot+app UF2: ${COMBINED_UF2}")
else()
	message(STATUS "MCUboot hex not found, skipping combined UF2")
endif()
