cmake_minimum_required(VERSION 3.20)

project(radxa_linkr_debugger_host_tests C)

enable_testing()

set(LINKR_DEBUGGER_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/../src)

function(linkr_debugger_add_host_test target)
  add_executable(${target} ${ARGN})
  target_compile_features(${target} PRIVATE c_std_11)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  target_include_directories(${target} PRIVATE ${LINKR_DEBUGGER_SRC_DIR})
  add_test(NAME ${target} COMMAND ${target})
endfunction()

linkr_debugger_add_host_test(linkr_debugger_model_rp2350_test
  ${LINKR_DEBUGGER_SRC_DIR}/linkr_debugger_model.c
  model_host/test_linkr_debugger_model.c
)
target_compile_definitions(linkr_debugger_model_rp2350_test PRIVATE CONFIG_SOC_SERIES_RP2350)

linkr_debugger_add_host_test(linkr_debugger_ota_parser_test
  ${LINKR_DEBUGGER_SRC_DIR}/linkr_debugger_ota.c
  model_host/test_linkr_debugger_ota.c
)
target_compile_definitions(linkr_debugger_ota_parser_test PRIVATE LINKR_DEBUGGER_OTA_HOST_TEST)

linkr_debugger_add_host_test(linkr_debugger_portal_test
  ${LINKR_DEBUGGER_SRC_DIR}/linkr_debugger_captive_portal.c
  model_host/test_linkr_debugger_portal.c
)
target_compile_definitions(linkr_debugger_portal_test PRIVATE
  LINKR_DEBUGGER_CAPTIVE_PORTAL_HOST_TEST
)

linkr_debugger_add_host_test(linkr_debugger_http_body_test
  ${LINKR_DEBUGGER_SRC_DIR}/linkr_debugger_http_body.c
  model_host/test_linkr_debugger_http_body.c
)

linkr_debugger_add_host_test(linkr_debugger_logic_analyzer_test
  ${LINKR_DEBUGGER_SRC_DIR}/linkr_debugger_logic_analyzer.c
  model_host/test_linkr_debugger_logic_analyzer.c
)
target_compile_definitions(linkr_debugger_logic_analyzer_test PRIVATE
  LINKR_DEBUGGER_LA_HOST_TEST
  CONFIG_SOC_SERIES_RP2350
)
