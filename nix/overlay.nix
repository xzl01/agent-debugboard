final: prev: {
  openocd-latest = final.callPackage ./openocd-latest.nix { };
  radxa-linkr-debuggerctl = final.callPackage ./package.nix { };
}
