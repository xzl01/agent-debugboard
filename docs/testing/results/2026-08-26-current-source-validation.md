# 2026-08-26 Current Source Validation

## Scope

This receipt binds the current dirty worktree used for the 2026-08-26 review.
The Saved Config FIXME remains documentation-only future UX debt; no Saved Config
redesign is included in this validation.

## Results

- `cargo test --manifest-path cmd-ng/Cargo.toml --all-targets`: **547 passed, 0 failed**.
- `cargo fmt --manifest-path cmd-ng/Cargo.toml --all --check`: **PASS**.
- `cargo clippy --manifest-path cmd-ng/Cargo.toml --all-targets -- -D warnings`: **PASS**.
- `nix flake check -L`: **PASS**; the Nix cargo check also completed with 547 tests.
- Web Node tests: **392 passed, 1 skipped**.
- Web Vitest: **430 passed**.
- Web production build with `wasm-bindgen-cli`: **PASS**.
- Repository governance, documentation, skill-boundary, persistent-configuration,
  and test-registration gates: **PASS**.
- Rust and Web LSP diagnostics: **no errors**.

## Hardware Evidence

- [Web GPIO real-board HIL](2026-08-26-web-gpio-final-hil.md): 30/30 functional
  assertions, HTTP/WS/CDC and BOOTSEL paths passed, baseline equality passed.
- [TUI busy-gesture HIL](2026-08-25-cmd-ng-tui-busy-gesture-final-hil.md): functional
  matrix passed with zero forwarded mutations.
- The protected PID `3731563` was absent at final checks, so release acceptance
  remains **FAIL** even though the functional HIL matrices passed.

## Scope Boundary

The GPIO malformed-output request behavior in the existing firmware sources is
pre-existing and outside this worktree diff; this host/Web change does not alter
firmware source or the HTTP wire contract.
