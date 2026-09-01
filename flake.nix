{
  description = "Radxa Linkr Debugger host CLI, skills, and documentation";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    { self
    , nixpkgs
    , flake-utils
    ,
    }:
    {
      overlays.default = import ./nix/overlay.nix;
    }
    // flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs {
          inherit system;
          overlays = [ self.overlays.default ];
        };
      in
      {
        packages = {
          openocd-latest = pkgs.openocd-latest;
          radxa-linkr-debuggerctl = pkgs.radxa-linkr-debuggerctl;
          default = pkgs.radxa-linkr-debuggerctl;
        };

        apps = {
          radxa-linkr-debuggerctl = {
            type = "app";
            program = "${pkgs.radxa-linkr-debuggerctl}/bin/radxa-linkr-debuggerctl";
            meta.description = "Run the Radxa Linkr Debugger CLI";
          };
          rdb = {
            type = "app";
            program = "${pkgs.radxa-linkr-debuggerctl}/bin/rdb";
            meta.description = "Run the Radxa Linkr Debugger CLI through its rdb alias";
          };
          default = self.apps.${system}.radxa-linkr-debuggerctl;
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ pkgs.radxa-linkr-debuggerctl ];
          packages = [
            pkgs.cargo
            pkgs.clippy
            pkgs.rustfmt
            pkgs.cargo-release
            pkgs.nixpkgs-fmt
            pkgs.pandoc
            pkgs.scdoc
          ];
        };

        formatter = pkgs.nixpkgs-fmt;

        checks = {
          build = pkgs.radxa-linkr-debuggerctl;
          openocd-latest = pkgs.openocd-latest;
          rdb-alias = pkgs.runCommand "radxa-linkr-debuggerctl-rdb-alias-check"
            {
              nativeBuildInputs = [ pkgs.diffutils ];
              SSL_CERT_FILE = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";
              NIX_SSL_CERT_FILE = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";
            } ''
            set -x
            echo "checking rdb symlink" >&2
            test -L "${pkgs.radxa-linkr-debuggerctl}/bin/rdb"
            expected_target="radxa-linkr-debuggerctl"
            actual_target="$("${pkgs.coreutils}/bin/readlink" "${pkgs.radxa-linkr-debuggerctl}/bin/rdb")"
            test "$actual_target" = "$expected_target"

            echo "checking CLI version parity" >&2
            primary_version="$TMPDIR/primary-version"
            rdb_version="$TMPDIR/rdb-version"
            "${pkgs.radxa-linkr-debuggerctl}/bin/radxa-linkr-debuggerctl" --version > "$primary_version"
            "${pkgs.radxa-linkr-debuggerctl}/bin/rdb" --version > "$rdb_version"
            cmp "$primary_version" "$rdb_version"
            echo "creating check output" >&2
            touch "$out"
          '';
          desktop-entry = pkgs.runCommand "radxa-linkr-debuggerctl-desktop-entry-check" { } ''
            package="${pkgs.radxa-linkr-debuggerctl}"
            desktop="$package/share/applications/radxa-linkr-debugger.desktop"
            icon="$package/share/icons/hicolor/scalable/apps/radxa-linkr-debugger.svg"
            test -x "$package/bin/linkr-host"
            test -x "$package/bin/linkr-tray"
            test -f "$package/share/radxa-linkr-debugger/web/index.html"
            test -s "$package/share/radxa-linkr-debugger/web/assets/decoder/logic-decoder.js"
            test -s "$package/share/radxa-linkr-debugger/web/assets/decoder/logic-decoder_bg.wasm"
            test -f "$desktop"
            test -f "$icon"
            ${pkgs.desktop-file-utils}/bin/desktop-file-validate "$desktop"
            grep -Fx "Name=Radxa Linkr Debugger" "$desktop"
            grep -Fx "GenericName=Hardware Debugger Control" "$desktop"
            grep -Fx "Keywords=Radxa;Linkr;Debugger;Embedded;" "$desktop"
            grep -Fx "Exec=radxa-linkr-debuggerctl" "$desktop"
            grep -Fx "TryExec=radxa-linkr-debuggerctl" "$desktop"
            grep -Fx "Icon=radxa-linkr-debugger" "$desktop"
            grep -Fx "Terminal=true" "$desktop"
            touch "$out"
          '';
          formatting = pkgs.runCommand "radxa-linkr-debugboard-flake-formatting-check" { } ''
            workdir="$TMPDIR/nix-format-check"
            mkdir -p "$workdir"

            cp ${./flake.nix} "$workdir/flake.nix"
            cp ${./nix/package.nix} "$workdir/package.nix"
            cp ${./nix/overlay.nix} "$workdir/overlay.nix"
            cp ${./nix/openocd-latest.nix} "$workdir/openocd-latest.nix"
            cp ${./shell.nix} "$workdir/shell.nix"

            chmod u+w "$workdir/flake.nix" "$workdir/package.nix" "$workdir/overlay.nix" "$workdir/openocd-latest.nix" "$workdir/shell.nix"

            before_flake=$(sha256sum "$workdir/flake.nix" | cut -d' ' -f1)
            before_package=$(sha256sum "$workdir/package.nix" | cut -d' ' -f1)
            before_overlay=$(sha256sum "$workdir/overlay.nix" | cut -d' ' -f1)
            before_openocd=$(sha256sum "$workdir/openocd-latest.nix" | cut -d' ' -f1)
            before_shell=$(sha256sum "$workdir/shell.nix" | cut -d' ' -f1)

            ${pkgs.nixpkgs-fmt}/bin/nixpkgs-fmt "$workdir/flake.nix" "$workdir/package.nix" "$workdir/overlay.nix" "$workdir/openocd-latest.nix" "$workdir/shell.nix"

            after_flake=$(sha256sum "$workdir/flake.nix" | cut -d' ' -f1)
            after_package=$(sha256sum "$workdir/package.nix" | cut -d' ' -f1)
            after_overlay=$(sha256sum "$workdir/overlay.nix" | cut -d' ' -f1)
            after_openocd=$(sha256sum "$workdir/openocd-latest.nix" | cut -d' ' -f1)
            after_shell=$(sha256sum "$workdir/shell.nix" | cut -d' ' -f1)

            test "$before_flake" = "$after_flake"
            test "$before_package" = "$after_package"
            test "$before_overlay" = "$after_overlay"
            test "$before_openocd" = "$after_openocd"
            test "$before_shell" = "$after_shell"

            touch "$out"
          '';
        };
      }
    );
}
