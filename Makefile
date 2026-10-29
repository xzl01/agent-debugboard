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

PICO2_TEST_BOARD := rpi_pico2/rp2350a/m33
PICO2_TEST_APP := apps/pico2_uart_swd_test
PICO2_TEST_BUILD_DIR := build/pico2_uart_swd_test

.PHONY: help firmware pico2-uart-swd-test pico2-test workspace unit-tests cli cli-test cli-clippy cli-fmt host-fmt host-clippy host-test decoder-fmt decoder-clippy decoder-test version-check web web-test web-build gates persistent-configuration-docs check all clean

# Keep canonical firmware and Web generation from racing under make -j; run
# each local validation lane once, in order, without rebuilding firmware in check.
.NOTPARALLEL: all check

help: ## Show this help
	@echo "Radxa Linkr Debugger build targets (run inside nix-shell):"
	@echo "  make firmware      full canonical firmware build (west + sysbuild)"
	@echo "  make pico2-test      Pico 2 bare board 2x UART bridge + CMSIS-DAP SWD test build"
	@echo "  make workspace     update the west workspace to the pinned manifest (once)"
	@echo "  make unit-tests    firmware host-model unit tests"
	@echo "  make cli           build the Rust host CLI"
	@echo "  make cli-test      run Rust host CLI tests"
	@echo "  make cli-clippy    clippy with -D warnings"
	@echo "  make cli-fmt       rustfmt check"
	@echo "  make host-fmt      Host/Broker/MCP/tray rustfmt check"
	@echo "  make host-clippy   Host clippy with -D warnings"
	@echo "  make host-test     locked Host tests (all targets)"
	@echo "  make decoder-fmt   Rust decoder rustfmt check"
	@echo "  make decoder-clippy Rust decoder clippy (all features)"
	@echo "  make decoder-test  locked native Rust decoder tests"
	@echo "  make version-check synchronized repository version check"
	@echo "  make web           web production build"
	@echo "  make web-test      web unit/contract tests"
	@echo "  make gates         repository gate checkers + their tests"
	@echo "  make check         local CI subset: gates/docs/version + CLI/Host/decoder + models + Web"
	@echo "  make all           firmware + check"
	@echo "  make clean         remove the build directory"

firmware: ## Full canonical firmware build into $(BUILD_DIR)
	$(NIX) "west build -p always -b $(BOARD) --sysbuild $(APP) -d $(BUILD_DIR)"

pico2-uart-swd-test: ## Bare Pico 2 UART bridges + CMSIS-DAP SWD test firmware
	$(NIX) "west build -p always -b $(PICO2_TEST_BOARD) $(PICO2_TEST_APP) -d $(PICO2_TEST_BUILD_DIR)"

pico2-test: pico2-uart-swd-test ## Alias for the Pico 2 test firmware

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

host-fmt: ## Host/Broker/MCP/tray rustfmt check
	$(NIX) "cargo fmt --manifest-path host-tools/Cargo.toml --all --check"

host-clippy: ## Host clippy with -D warnings
	$(NIX) "cargo clippy --locked --manifest-path host-tools/Cargo.toml --all-targets -- -D warnings"

host-test: ## Locked Host tests
	$(NIX) "cargo test --locked --manifest-path host-tools/Cargo.toml --all-targets"

decoder-fmt: ## Rust decoder rustfmt check
	$(NIX) "cargo fmt --manifest-path web/decoder/Cargo.toml --all --check"

decoder-clippy: ## Rust decoder clippy, including WASM feature checks
	$(NIX) "cargo clippy --locked --manifest-path web/decoder/Cargo.toml --all-targets --all-features -- -D warnings"

decoder-test: ## Locked native Rust decoder tests (Web build covers WASM output)
	$(NIX) "cargo test --locked --manifest-path web/decoder/Cargo.toml --all-targets"

version-check: ## Synchronized repository version check
	$(NIX) "python3 scripts/version_sync.py check"

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
	scripts/install-host.test.mjs \
	scripts/verify-frozen-evidence.test.mjs" && \
	$(NIX) "node scripts/check-nightly-workflow.mjs --root ." && \
	$(NIX) "node scripts/check-rdb-alias.mjs --root ." && \
	$(NIX) "node scripts/check-repository-gates.mjs --root ." && \
	$(NIX) "node scripts/check-test-registration.mjs --root ." && \
	$(NIX) "node scripts/check-doc-layout.mjs --root ." && \
	$(NIX) "node scripts/check-skill-boundary.mjs --root ."

persistent-configuration-docs: ## Persistent-configuration documentation contract
	$(NIX) "cargo fetch --locked --manifest-path cmd-ng/Cargo.toml" && \
	$(NIX) "node --test scripts/check-persistent-configuration-docs.test.mjs" && \
	$(NIX) "node scripts/check-persistent-configuration-docs.mjs --root ."

check: gates persistent-configuration-docs version-check cli-fmt cli-clippy cli-test host-fmt host-clippy host-test decoder-fmt decoder-clippy decoder-test unit-tests web-test web ## Local CI subset, no firmware build

all: firmware check ## Firmware build + full checks

clean: ## Remove the build directory
	rm -rf build
