# Radxa Linkr Debugger — unified build orchestration
#
# Every recipe runs inside the project nix shell (shell.nix). Use either
#   make firmware
# from inside `nix-shell`, or
#   NIX="nix-shell --run" make firmware
# from a plain shell. The one-shot form is documented in
# docs/developer/build.md.

SHELL := /bin/sh

NIX ?= nix-shell --run

BOARD := rpi_pico2/rp2350a/m33/mcuboot
APP := apps/radxa_linkr_debugger
BUILD_DIR := build/radxa_linkr_debugger

.PHONY: help firmware workspace unit-tests
help: ## Show this help
	@echo "Radxa Linkr Debugger build targets (run inside nix-shell):"
	@echo "  make firmware      full canonical firmware build (west + sysbuild)"
	@echo "  make workspace     update the west workspace to the pinned manifest (once)"
	@echo "  make unit-tests    firmware host-model unit tests"
firmware: ## Full canonical firmware build into $(BUILD_DIR)
	$(NIX) "west build -p always -b $(BOARD) --sysbuild $(APP) -d $(BUILD_DIR)"

workspace: ## Update the west workspace to the pinned manifest (once)
	$(NIX) "west update --narrow -o=--depth=1"

unit-tests: ## Firmware host-model unit tests
	$(NIX) "CC=gcc $(APP)/tests/run_unit_tests.sh"
