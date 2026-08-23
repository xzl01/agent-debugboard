{ pkgs ? import <nixpkgs> { } }:

let
  # Zephyr SDK packaging from https://github.com/nix-community/zephyr-nix,
  # pinned for reproducibility. Used in classic (non-flake) mode, following
  # the upstream shell.nix example:
  #   packages = [ (zephyr.sdk.override { targets = [ "arm-zephyr-eabi" ]; }) ];
  zephyr-nix = pkgs.callPackage
    (builtins.fetchTarball {
      url = "https://github.com/nix-community/zephyr-nix/archive/6966fb1cbf2fdb494bea3062c5e8e7d44dd8ac9c.tar.gz";
      sha256 = "0h3y6cqsckyawc9m49bfwh9xprzndqv28isdg7ln5chgac8cxfp3";
    })
    {
      # zephyr-src/pyproject-nix are only needed by zephyr-nix.pythonEnv, which
      # we intentionally do not use: this repo builds against its west-pinned
      # Zephyr checkout, so the Python environment is assembled from nixpkgs
      # below instead. Nix laziness means these nulls are never evaluated.
      zephyr-src = null;
      pyproject-nix = null;
      # Required by SDK >= 1.0 (GDB Python bindings).
      python312 = pkgs.python312;
    };

  pythonWithZephyr = pkgs.python3.withPackages (ps: with ps; [
    west
    intelhex
    click
    cbor2
    cryptography
    pyelftools
    pyyaml
    pykwalify
    jsonschema
    packaging
    psutil
    requests
    semver
    tqdm
    anytree
    canopen
    pylink-square
    pyserial
  ]);

  openocdLatest = pkgs.callPackage ./nix/openocd-latest.nix { };
in
pkgs.mkShell {
  packages = [
    (zephyr-nix.sdk.override {
      targets = [ "arm-zephyr-eabi" ];
    })
    pkgs.cmake
    pkgs.ninja
    pkgs.dtc
    pkgs.gperf
    pkgs.pkg-config
    pkgs.udev
    pythonWithZephyr
    pkgs.nodejs_22
    pkgs.cargo
    pkgs.rustc
    pkgs.clippy
    pkgs.rustfmt
    pkgs.clang
    pkgs.lld
    pkgs.wasm-bindgen-cli
    openocdLatest
    pkgs.picotool
    pkgs.udisks2
    pkgs.wget
  ];

  CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER = "clang";

  shellHook = ''
    # The Zephyr build system locates the toolchain via ZEPHYR_SDK_INSTALL_DIR
    # (exported by the sdk's setup-hook); also expose it on PATH for
    # interactive use (gdb, objdump, ...).
    export PATH="$ZEPHYR_SDK_INSTALL_DIR/gnu/arm-zephyr-eabi/bin:$PATH"
    echo "[shell.nix] Zephyr SDK: $ZEPHYR_SDK_INSTALL_DIR"
    echo "[shell.nix] arm-zephyr-eabi-gcc: $(command -v arm-zephyr-eabi-gcc 2>/dev/null || echo NOT FOUND)"
  '';
}
