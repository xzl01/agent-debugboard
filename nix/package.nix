{ lib
, rustPlatform
, cacert
, installShellFiles
, pkg-config
, udev
,
}:

let
  version = "0.3.0";
in
rustPlatform.buildRustPackage {
  pname = "radxa-linkr-debuggerctl";
  inherit version;

  src = lib.cleanSource ./..;

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
    ln -s radxa-linkr-debuggerctl "$out/bin/rdb"
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

  meta = {
    description = "Released Rust CLI/TUI and desktop launcher for Radxa Linkr Debugger";
    homepage = "https://github.com/xzl01/agent-debugboard";
    license = lib.licenses.lgpl3Plus;
    mainProgram = "radxa-linkr-debuggerctl";
    platforms = lib.platforms.unix;
  };
}
