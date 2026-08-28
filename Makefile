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

.PHONY: help firmware workspace unit-tests cli cli-test cli-clippy cli-fmt web web-test web-build gates persistent-configuration-docs check all clean

help: ## Show this help
	@echo "Radxa Linkr Debugger build targets (run inside nix-shell):"
	@echo "  make firmware      full canonical firmware build (west + sysbuild)"
	@echo "  make workspace     update the west workspace to the pinned manifest (once)"
	@echo "  make unit-tests    firmware host-model unit tests"
	@echo "  make cli           build the Rust host CLI"
	@echo "  make cli-test      run Rust host CLI tests"
	@echo "  make cli-clippy    clippy with -D warnings"
	@echo "  make cli-fmt       rustfmt check"
	@echo "  make web           web production build"
	@echo "  make web-test      web unit/contract tests"
	@echo "  make gates         repository gate checkers + their tests"
	@echo "  make check         gates + persistent docs + CLI + firmware model + Web tests/build"
	@echo "  make all           firmware + check"
	@echo "  make clean         remove the build directory"

firmware: ## Full canonical firmware build into $(BUILD_DIR)
	$(NIX) "west build -p always -b $(BOARD) --sysbuild $(APP) -d $(BUILD_DIR)"

workspace: ## Update the west workspace to the pinned manifest (once)
	$(NIX) "west update --narrow -o=--depth=1"

unit-tests: ## Firmware host-model unit tests
	$(NIX) "CC=gcc $(APP)/tests/run_unit_tests.sh"

cli: ## Build the Rust host CLI/TUI
	$(NIX) "cargo build --manifest-path cmd-ng/Cargo.toml"

cli-test: ## Rust host CLI tests
	$(NIX) "cargo test --manifest-path cmd-ng/Cargo.toml"

cli-clippy: ## Clippy with -D warnings
	$(NIX) "cargo clippy --manifest-path cmd-ng/Cargo.toml --all-targets -- -D warnings"

cli-fmt: ## rustfmt check
	$(NIX) "cargo fmt --manifest-path cmd-ng/Cargo.toml --all --check"

web: ## Web production build
	$(NIX) "cd web && npm run build"

web-test: ## Web unit/contract tests
	$(NIX) "cd web && npm test"

gates: ## Repository gate checkers and their contract tests
	$(NIX) "node --test scripts/check-nightly-workflow.test.mjs \
	scripts/check-rdb-alias.test.mjs \
	scripts/check-repository-gates.test.mjs \
	scripts/check-test-registration.test.mjs \
	scripts/check-doc-layout.test.mjs \
	scripts/check-skill-boundary.test.mjs \
	scripts/check-worktree-scope.test.mjs \
	scripts/verify-frozen-evidence.test.mjs" && \
	$(NIX) "node scripts/check-nightly-workflow.mjs --root ." && \
	$(NIX) "node scripts/check-rdb-alias.mjs --root ." && \
	$(NIX) "node scripts/check-repository-gates.mjs --root ." && \
	$(NIX) "node scripts/check-test-registration.mjs --root ." && \
	$(NIX) "node scripts/check-doc-layout.mjs --root ." && \
	$(NIX) "node scripts/check-skill-boundary.mjs --root ."

persistent-configuration-docs: ## Persistent-configuration documentation contract
	$(NIX) "node --test scripts/check-persistent-configuration-docs.test.mjs" && \
	$(NIX) "node scripts/check-persistent-configuration-docs.mjs --root ."

check: gates persistent-configuration-docs cli-fmt cli-clippy cli-test unit-tests web-test web ## Everything except the firmware build

all: firmware check ## Firmware build + full checks

clean: ## Remove the build directory
	rm -rf build
