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
          radxa-linkr-debuggerctl = pkgs.radxa-linkr-debuggerctl;
          default = pkgs.radxa-linkr-debuggerctl;
        };

        apps = {
          radxa-linkr-debuggerctl = {
            type = "app";
            program = "${pkgs.radxa-linkr-debuggerctl}/bin/radxa-linkr-debuggerctl";
            meta.description = "Run the Radxa Linkr Debugger CLI";
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
          formatting = pkgs.runCommand "radxa-linkr-debugboard-flake-formatting-check" { } ''
            workdir="$TMPDIR/nix-format-check"
            mkdir -p "$workdir"

            cp ${./flake.nix} "$workdir/flake.nix"
            cp ${./nix/package.nix} "$workdir/package.nix"
            cp ${./nix/overlay.nix} "$workdir/overlay.nix"

            chmod u+w "$workdir/flake.nix" "$workdir/package.nix" "$workdir/overlay.nix"

            before_flake=$(sha256sum "$workdir/flake.nix" | cut -d' ' -f1)
            before_package=$(sha256sum "$workdir/package.nix" | cut -d' ' -f1)
            before_overlay=$(sha256sum "$workdir/overlay.nix" | cut -d' ' -f1)

            ${pkgs.nixpkgs-fmt}/bin/nixpkgs-fmt "$workdir/flake.nix" "$workdir/package.nix" "$workdir/overlay.nix"

            after_flake=$(sha256sum "$workdir/flake.nix" | cut -d' ' -f1)
            after_package=$(sha256sum "$workdir/package.nix" | cut -d' ' -f1)
            after_overlay=$(sha256sum "$workdir/overlay.nix" | cut -d' ' -f1)

            test "$before_flake" = "$after_flake"
            test "$before_package" = "$after_package"
            test "$before_overlay" = "$after_overlay"

            touch "$out"
          '';
        };
      }
    );
}
