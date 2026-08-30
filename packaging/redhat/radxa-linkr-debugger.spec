%global upstream_version 0.3.0
%global debug_package %{nil}

%ifarch x86_64
%global release_arch amd64
%endif
%ifarch aarch64
%global release_arch arm64
%endif

Name:           radxa-linkr-debuggerctl
Version:        %{lua:print((rpm.expand("%{upstream_version}"):gsub("-", "~")))}
Release:        1%{?dist}
Summary:        Command-line tools for Radxa Linkr Debugger
License:        LGPL-3.0-or-later
URL:            https://github.com/xzl01/agent-debugboard
ExclusiveArch:  x86_64 aarch64
Requires:       ca-certificates

Source0:        %{url}/releases/download/v%{upstream_version}/agent-debugboard-%{upstream_version}.tar.gz
Source1:        %{url}/releases/download/v%{upstream_version}/radxa-linkr-debuggerctl-rust_linux_%{release_arch}.tar.gz
Source2:        %{url}/releases/download/v%{upstream_version}/radxa-linkr-debugger-rp2350.uf2
Source3:        %{url}/releases/download/v%{upstream_version}/radxa-linkr-debugger-rp2350-ota.bin

%description
Standalone CLI/TUI for controlling Radxa Linkr Debugger hardware.

%package -n radxa-linkr-debugger-firmware
Summary:        Firmware images for Radxa Linkr Debugger
BuildArch:      noarch
Suggests:       radxa-linkr-debuggerctl

%description -n radxa-linkr-debugger-firmware
Combined MCUboot and application UF2 for ROM BOOTSEL recovery, plus the
unsigned MCUboot-format application image for OTA updates.

%prep
%autosetup -n agent-debugboard-%{upstream_version}
mkdir -p .native-cli
tar -xzf %{SOURCE1} -C .native-cli
test -x .native-cli/radxa-linkr-debuggerctl
test ! -e .native-cli/zephyr.uf2

%build

%install
install -Dm755 .native-cli/radxa-linkr-debuggerctl \
    %{buildroot}%{_bindir}/radxa-linkr-debuggerctl
ln -s radxa-linkr-debuggerctl %{buildroot}%{_bindir}/rdb
install -Dm644 %{SOURCE2} \
    %{buildroot}%{_datadir}/radxa-linkr-debugger/firmware/radxa-linkr-debugger-rp2350.uf2
install -Dm644 %{SOURCE3} \
    %{buildroot}%{_datadir}/radxa-linkr-debugger/firmware/radxa-linkr-debugger-rp2350-ota.bin

%check
test "$(.native-cli/radxa-linkr-debuggerctl --version)" = \
    "radxa-linkr-debuggerctl %{upstream_version}"
.native-cli/radxa-linkr-debuggerctl --help >/dev/null
test -s %{SOURCE2}
test -s %{SOURCE3}
test ! -e zephyr.uf2

%files
%license LICENSE COPYING COPYING.LESSER
%doc README.md NOTICE docs/user/cli.md docs/user/cli.zh-CN.md
%{_bindir}/radxa-linkr-debuggerctl
%{_bindir}/rdb

%files -n radxa-linkr-debugger-firmware
%license LICENSE COPYING COPYING.LESSER
%doc NOTICE docs/user/install.md docs/user/install.zh-CN.md
%{_datadir}/radxa-linkr-debugger/firmware/

%changelog
* Sun Aug 30 2026 Jiali Chen <chenjiali@radxa.com> - 0.3.0-1
- Package the standalone CLI and safe flashable firmware separately.
