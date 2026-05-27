{
  description = "Radxa Linkr Debugger host CLI, skills, and documentation";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.11";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
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
      }
    );
}
