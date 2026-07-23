{ pkgs ? import <nixpkgs> {} }:

let
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

  zephyrSdk = pkgs.callPackage ./nix/zephyr-sdk.nix {
    targets = [ "arm-zephyr-eabi" ];
  };
in
pkgs.mkShell {
  packages = [
    zephyrSdk
    pkgs.cmake
    pkgs.ninja
    pkgs.dtc
    pkgs.gperf
    pythonWithZephyr
    pkgs.nodejs_22
    # Rust comes from rustup (see README): the wasm32-unknown-unknown target
    # std is required for the decoder and nixpkgs rustc cannot provide it.
    # clang acts as the rustup host linker because rustup's bundled ld
    # wrapper references garbage-collected nix store paths.
    pkgs.clang
    pkgs.wasm-bindgen-cli
    pkgs.picotool
    pkgs.udisks2
    pkgs.wget
    pkgs.chromium
  ];

  CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER = "clang";
  PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH = "${pkgs.chromium}/bin/chromium";

  shellHook = ''
    export PATH="$ZEPHYR_SDK_INSTALL_DIR/gnu/arm-zephyr-eabi/bin:$PATH"
    echo "[shell.nix] Zephyr SDK: $ZEPHYR_SDK_INSTALL_DIR"
    echo "[shell.nix] arm-zephyr-eabi-gcc: $(command -v arm-zephyr-eabi-gcc 2>/dev/null || echo NOT FOUND)"
  '';
}
