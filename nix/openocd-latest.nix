{ openocd
, fetchFromGitHub
, autoreconfHook
,
}:

openocd.overrideAttrs (old: {
  version = "0.12.0+dev-2026-07-28";
  src = fetchFromGitHub {
    owner = "openocd-org";
    repo = "openocd";
    rev = "da3920b0a52dc2d394afb222c688dac7e57acc1b";
    hash = "sha256-osZAASRIUDMbDhbH6lIuyx5KtKP7MYaj+WlD6EWpIEo=";
  };
  nativeBuildInputs = old.nativeBuildInputs ++ [ autoreconfHook ];
  configureFlags = old.configureFlags ++ [
    "--enable-ch347"
  ];
  postInstall = (old.postInstall or "") + ''
    "$out/bin/openocd" -c "adapter list" -c shutdown 2>&1 \
      | grep -Eq '(^|[[:space:]])ch347([[:space:]]|$)'
  '';
})
