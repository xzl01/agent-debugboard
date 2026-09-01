{ lib
, stdenvNoCC
, rustPlatform
, buildNpmPackage
, cacert
, installShellFiles
, lld
, pkg-config
, udev
, gtk3
, libayatana-appindicator
, wrapGAppsHook3
, wasm-bindgen-cli_0_2_121
,
}:

let
  version = "0.3.0";
  wasmTarget = "wasm32-unknown-unknown";
  src = lib.fileset.toSource {
    root = ./..;
    fileset = lib.fileset.unions [
      ../cmd-ng
      ../host-tools
      ../web
      ../packaging/radxa-linkr-debugger.desktop
      ../skills/radxa-linkr-debugger
      ../docs/reference/openocd/README.md
      ../docs/testing/hil-functional-test-spec.md
    ];
  };

  cli = rustPlatform.buildRustPackage {
    pname = "radxa-linkr-debuggerctl-cli";
    inherit version src;

    cargoRoot = "cmd-ng";
    cargoHash = "sha256-fF0hOW3MExveRvJCzfAOV44gn8XhNv93P6xVrNzzp8o=";
    buildAndTestSubdir = "cmd-ng";

    nativeBuildInputs = [
      installShellFiles
      pkg-config
    ];

    buildInputs = [
      cacert
      udev
    ];

    SSL_CERT_FILE = "${cacert}/etc/ssl/certs/ca-bundle.crt";
    NIX_SSL_CERT_FILE = "${cacert}/etc/ssl/certs/ca-bundle.crt";

    postInstall = ''
      install -Dm644 packaging/radxa-linkr-debugger.desktop \
        "$out/share/applications/radxa-linkr-debugger.desktop"
      install -Dm644 web/public/linkr-mark.svg \
        "$out/share/icons/hicolor/scalable/apps/radxa-linkr-debugger.svg"
      install -Dm644 skills/radxa-linkr-debugger/SKILL.md \
        "$out/share/radxa-linkr-debugger/skills/radxa-linkr-debugger/SKILL.md"
      install -Dm755 skills/radxa-linkr-debugger/scripts/install.sh \
        "$out/share/radxa-linkr-debugger/skills/radxa-linkr-debugger/scripts/install.sh"
      install -Dm755 skills/radxa-linkr-debugger/scripts/install.ps1 \
        "$out/share/radxa-linkr-debugger/skills/radxa-linkr-debugger/scripts/install.ps1"
      install -Dm644 docs/reference/openocd/README.md \
        "$out/share/doc/radxa-linkr-debugger/openocd.md"
      install -Dm644 docs/testing/hil-functional-test-spec.md \
        "$out/share/doc/radxa-linkr-debugger/testing/hil-functional-test-spec.md"

      if [ -d man ]; then
        installManPage man/*
      fi
    '';
  };

  host = rustPlatform.buildRustPackage {
    pname = "radxa-linkr-debugger-host";
    inherit version src;

    cargoRoot = "host-tools";
    cargoHash = "sha256-WehQDM4mpDfRPIhO5GsoH3TKkH4FH9KAMu9/HUysx4s=";
    buildAndTestSubdir = "host-tools";

    nativeBuildInputs = [ pkg-config ];
    buildInputs = [
      cacert
      gtk3
      udev
    ];

    SSL_CERT_FILE = "${cacert}/etc/ssl/certs/ca-bundle.crt";
    NIX_SSL_CERT_FILE = "${cacert}/etc/ssl/certs/ca-bundle.crt";
  };

  decoder = rustPlatform.buildRustPackage {
    pname = "radxa-linkr-debugger-decoder";
    inherit version src;

    cargoRoot = "web/decoder";
    cargoHash = "sha256-0onsUZbw7SYV0t+N7Pfx/Fknm8xaGp0EznzanZIaLTQ=";
    buildAndTestSubdir = "web/decoder";

    nativeBuildInputs = [
      lld
      wasm-bindgen-cli_0_2_121
    ];
    doCheck = false;

    buildPhase = ''
      runHook preBuild
      export CARGO_TARGET_DIR="$PWD/target"
      cargo build \
        --manifest-path web/decoder/Cargo.toml \
        --frozen \
        --release \
        --target ${wasmTarget} \
        --features wasm \
        -j "$NIX_BUILD_CORES"
      runHook postBuild
    '';

    installPhase = ''
      runHook preInstall
      wasm-bindgen \
        "target/${wasmTarget}/release/radxa_logic_decoder.wasm" \
        --target web \
        --out-dir "$out" \
        --out-name logic-decoder \
        --no-typescript
      runHook postInstall
    '';
  };

  web = buildNpmPackage {
    pname = "radxa-linkr-debugger-web";
    inherit version;
    src = "${src}/web";

    npmDepsHash = "sha256-ZmawReW5hU932UuNFiooGZOyo6Bq1QPPAuCIzq+QQ9o=";

    buildPhase = ''
      runHook preBuild
      (
        ./node_modules/.bin/tsc -b
        ./node_modules/.bin/vite build --config vite.config.ts
        install -Dm644 ${decoder}/logic-decoder.js \
          dist/assets/decoder/logic-decoder.js
        install -Dm644 ${decoder}/logic-decoder_bg.wasm \
          dist/assets/decoder/logic-decoder_bg.wasm
      )
      runHook postBuild
    '';

    installPhase = ''
      runHook preInstall
      mkdir -p "$out/share/radxa-linkr-debugger/web"
      cp -R dist/. "$out/share/radxa-linkr-debugger/web/"
      runHook postInstall
    '';
  };
in
stdenvNoCC.mkDerivation {
  pname = "radxa-linkr-debuggerctl";
  inherit version;

  dontUnpack = true;
  dontWrapGApps = true;

  nativeBuildInputs = [ wrapGAppsHook3 ];
  buildInputs = [
    gtk3
    libayatana-appindicator
  ];

  installPhase = ''
    runHook preInstall
    mkdir -p "$out/bin" "$out/share"
    ln -s ${cli}/bin/radxa-linkr-debuggerctl "$out/bin/radxa-linkr-debuggerctl"
    ln -s radxa-linkr-debuggerctl "$out/bin/rdb"
    ln -s ${host}/bin/linkr-host "$out/bin/linkr-host"
    ln -s ${host}/bin/linkr-tray "$out/bin/linkr-tray"
    mkdir -p "$out/share/radxa-linkr-debugger"
    cp -Rs ${web}/share/radxa-linkr-debugger/web \
      "$out/share/radxa-linkr-debugger/web"
    cp -Rs ${cli}/share/. "$out/share/"
    runHook postInstall
  '';

  preFixup = ''
    gappsWrapperArgs+=(
      --set LINKR_TRAY_BIN "$out/bin/linkr-tray"
      --set LINKR_WEB_ROOT "$out/share/radxa-linkr-debugger/web"
      --prefix LD_LIBRARY_PATH : "${lib.makeLibraryPath [ libayatana-appindicator ]}"
    )
  '';

  postFixup = ''
    wrapGApp "$out/bin/radxa-linkr-debuggerctl"
    wrapGApp "$out/bin/linkr-host"
    wrapGApp "$out/bin/linkr-tray"
  '';

  meta = {
    description = "Radxa Linkr Debugger CLI/TUI and desktop stack";
    homepage = "https://github.com/xzl01/agent-debugboard";
    license = lib.licenses.lgpl3Plus;
    mainProgram = "radxa-linkr-debuggerctl";
    platforms = lib.platforms.linux;
  };
}
