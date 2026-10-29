//! USB identity only: no board pin, connector, or rail knowledge belongs here.
use tokio_serial::{SerialPortInfo, SerialPortType};

#[cfg(target_os = "macos")]
#[path = "serial_discovery/macos.rs"]
mod macos;
#[cfg(target_os = "windows")]
#[path = "serial_discovery/windows.rs"]
mod windows;

const CH347_VID: u16 = 0x1a86;
const CH347F_PID: u16 = 0x55de;

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Identity {
    pub(crate) device: String,
    pub(crate) serial: String,
    pub(crate) interface: u8,
    // Native callout/dialin aliases may share one IOKit serial service. Only the
    // OS-advertised callout path is admitted, never a guessed filename prefix.
    canonical_port: String,
}

#[derive(Clone)]
struct NativeIdentity {
    device: String,
    serial: String,
    vid: u16,
    pid: u16,
    interface: u8,
    cdc_data_interface: bool,
    canonical_port: String,
}

fn verify_native(port: &SerialPortInfo, native: NativeIdentity) -> Result<Identity, String> {
    let SerialPortType::UsbPort(info) = &port.port_type else {
        return Err("not a USB port".into());
    };
    if native.device.is_empty()
        || native.serial.is_empty()
        || native.canonical_port.is_empty()
        || native.vid != CH347_VID
        || native.pid != CH347F_PID
        || info.vid != native.vid
        || info.pid != native.pid
        || info
            .serial_number
            .as_deref()
            .is_some_and(|serial| serial != native.serial)
        || info
            .interface
            .is_some_and(|interface| interface != native.interface)
    {
        return Err(format!(
            "USB enumeration and native device/interface identity disagree for {}",
            port.port_name
        ));
    }
    let interface = match (native.interface, native.cdc_data_interface) {
        (0 | 2, false) => native.interface,
        (1 | 3, true) => native.interface - 1,
        _ => return Err("unsupported CH347F CDC interface identity".into()),
    };
    Ok(Identity {
        device: native.device,
        serial: native.serial,
        interface,
        canonical_port: native.canonical_port,
    })
}

// These are Windows USB instance IDs from Configuration Manager, not COM names.
#[cfg(any(target_os = "windows", test))]
fn windows_usb_instance(id: &str) -> Option<(u16, u16, Option<u8>, &str)> {
    let mut parts = id.split('\\');
    if !parts.next()?.eq_ignore_ascii_case("USB") {
        return None;
    }
    let hardware = parts.next()?.to_ascii_uppercase();
    let instance = parts.next()?;
    if instance.is_empty() || parts.next().is_some() {
        return None;
    }
    let fields: Vec<_> = hardware.split('&').collect();
    if !(2..=3).contains(&fields.len()) {
        return None;
    }
    let vid = fields[0]
        .strip_prefix("VID_")
        .filter(|value| value.len() == 4)?;
    let pid = fields[1]
        .strip_prefix("PID_")
        .filter(|value| value.len() == 4)?;
    let interface = if fields.len() == 3 {
        let value = fields[2]
            .strip_prefix("MI_")
            .filter(|value| value.len() == 2)?;
        Some(u8::from_str_radix(value, 16).ok()?)
    } else {
        None
    };
    Some((
        u16::from_str_radix(vid, 16).ok()?,
        u16::from_str_radix(pid, 16).ok()?,
        interface,
        instance,
    ))
}

#[derive(Clone, Debug)]
pub(crate) struct Candidate {
    pub(crate) port: SerialPortInfo,
    pub(crate) identity: Identity,
}

fn is_target(port: &SerialPortInfo) -> bool {
    matches!(&port.port_type, SerialPortType::UsbPort(info)
        if info.vid == CH347_VID && info.pid == CH347F_PID)
}

pub(crate) fn select(
    ports: Vec<SerialPortInfo>,
    interface: u8,
    serial: Option<&str>,
    identify: impl Fn(&SerialPortInfo) -> Result<Identity, String>,
) -> Result<Candidate, String> {
    let mut candidates = Vec::new();
    for port in ports.into_iter().filter(is_target) {
        // The OS USB descriptor must independently identify the device/interface.
        let identity = identify(&port)?;
        if port.port_name != identity.canonical_port {
            continue;
        }
        if serial.is_none_or(|serial| serial == identity.serial) {
            candidates.push(Candidate { port, identity });
        }
    }
    let Some(first) = candidates.first() else {
        return Err("no verified CH347F USB device found".to_owned());
    };
    // CH347F PID 55de exposes two CDC functions: communication interfaces 0/2.
    // Require the complete pair on one physical USB device, never sorted indices.
    if candidates.len() != 2
        || candidates.iter().any(|candidate| {
            candidate.identity.device != first.identity.device
                || candidate.identity.serial != first.identity.serial
        })
        || candidates
            .iter()
            .filter(|candidate| candidate.identity.interface == 0)
            .count()
            != 1
        || candidates
            .iter()
            .filter(|candidate| candidate.identity.interface == 2)
            .count()
            != 1
        || candidates[0].port.port_name == candidates[1].port.port_name
    {
        return Err(
            "missing or ambiguous CH347F device/interface identity; select a unique device_serial"
                .to_owned(),
        );
    }
    candidates
        .into_iter()
        .find(|candidate| candidate.identity.interface == interface)
        .ok_or_else(|| "unsupported CH347F interface".to_owned())
}

pub(crate) fn discover(interface: u8, serial: Option<&str>) -> Result<Candidate, String> {
    let ports = tokio_serial::available_ports().map_err(|error| error.to_string())?;
    select(ports, interface, serial, identify)
}

fn identify(port: &SerialPortInfo) -> Result<Identity, String> {
    verify_native(port, native_identity(port)?)
}

#[cfg(target_os = "macos")]
use macos::native_identity;
#[cfg(target_os = "windows")]
use windows::native_identity;

#[cfg(target_os = "linux")]
fn native_identity(port: &SerialPortInfo) -> Result<NativeIdentity, String> {
    use std::{fs, path::Path};
    let fail = || {
        format!(
            "cannot verify USB device/interface identity for {}",
            port.port_name
        )
    };
    let name = Path::new(&port.port_name).file_name().ok_or_else(fail)?;
    let path = fs::canonicalize(Path::new("/sys/class/tty").join(name).join("device"))
        .map_err(|_| fail())?;
    let interface_path = path
        .ancestors()
        .find(|path| path.join("bInterfaceNumber").is_file())
        .ok_or_else(fail)?;
    let device = interface_path.parent().ok_or_else(fail)?;
    let hex = |path| {
        fs::read_to_string(path)
            .ok()
            .and_then(|text| u16::from_str_radix(text.trim(), 16).ok())
    };
    let vid = hex(device.join("idVendor")).ok_or_else(fail)?;
    let pid = hex(device.join("idProduct")).ok_or_else(fail)?;
    let interface = hex(interface_path.join("bInterfaceNumber"))
        .and_then(|value| u8::try_from(value).ok())
        .ok_or_else(fail)?;
    let serial = fs::read_to_string(device.join("serial"))
        .map_err(|_| fail())?
        .trim()
        .to_owned();
    Ok(NativeIdentity {
        device: device.to_string_lossy().into_owned(),
        serial,
        vid,
        pid,
        interface,
        cdc_data_interface: false,
        canonical_port: port.port_name.clone(),
    })
}

#[cfg(not(any(target_os = "linux", target_os = "macos", target_os = "windows")))]
fn native_identity(_port: &SerialPortInfo) -> Result<NativeIdentity, String> {
    Err(
        "CH347F interface identity is unavailable on this platform; refusing automatic selection"
            .to_owned(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn native_fixture(
        port: &str,
        device: &str,
        raw: u8,
        data: bool,
    ) -> (SerialPortInfo, NativeIdentity) {
        let mut info = self::port(port, CH347F_PID);
        if let SerialPortType::UsbPort(usb) = &mut info.port_type {
            usb.interface = Some(raw);
        }
        (
            info,
            NativeIdentity {
                device: device.into(),
                serial: "test".into(),
                vid: CH347_VID,
                pid: CH347F_PID,
                interface: raw,
                cdc_data_interface: data,
                canonical_port: port.into(),
            },
        )
    }

    #[test]
    fn native_identity_fixtures_require_physical_parent_and_validate_interfaces() {
        for (device, raw0, raw1, data) in [
            ("sysfs:/devices/usb1/1-2", 0, 2, false),
            ("windows:42:USB\\VID_1A86&PID_55DE\\test", 0, 2, false),
            ("iokit:12345", 1, 3, true),
        ] {
            let (a, native_a) = native_fixture("arbitrary-a", device, raw0, data);
            let (b, native_b) = native_fixture("arbitrary-b", device, raw1, data);
            let identify = |port: &SerialPortInfo| {
                verify_native(
                    port,
                    if port.port_name == a.port_name {
                        native_a.clone()
                    } else {
                        native_b.clone()
                    },
                )
            };
            assert_eq!(
                select(vec![b.clone(), a.clone()], 0, None, identify)
                    .unwrap()
                    .port
                    .port_name,
                a.port_name
            );
            let mut other_parent = native_b.clone();
            other_parent.device.push_str("-different-physical-parent");
            assert!(select(vec![a.clone(), b.clone()], 0, Some("test"), |port| {
                verify_native(
                    port,
                    if port.port_name == a.port_name {
                        native_a.clone()
                    } else {
                        other_parent.clone()
                    },
                )
            })
            .is_err());
            let mut mismatch = native_a.clone();
            mismatch.interface = raw1;
            assert!(verify_native(&a, mismatch).is_err());
            let mut missing = native_a.clone();
            missing.device.clear();
            assert!(verify_native(&a, missing).is_err());
            // Native ancestry remains authoritative when enumeration omits a field.
            let mut sparse = a.clone();
            if let SerialPortType::UsbPort(usb) = &mut sparse.port_type {
                usb.interface = None;
                usb.serial_number = None;
            }
            assert!(verify_native(&sparse, native_a.clone()).is_ok());
            let mut wrong_pid = native_a.clone();
            wrong_pid.pid = 0x7523;
            assert!(verify_native(&a, wrong_pid).is_err());
        }
    }

    #[test]
    fn macos_data_interfaces_and_native_dialin_aliases_are_not_extra_devices() {
        let (a, native_a) = native_fixture("callout-alpha", "iokit:1", 1, true);
        let (b, native_b) = native_fixture("callout-beta", "iokit:1", 3, true);
        let mut alias_a = a.clone();
        alias_a.port_name = "dialin-alpha".into();
        let mut alias_b = b.clone();
        alias_b.port_name = "dialin-beta".into();
        let selected = select(
            vec![alias_a, b.clone(), alias_b, a.clone()],
            2,
            None,
            |port| {
                verify_native(
                    port,
                    match port.port_name.as_str() {
                        "callout-alpha" | "dialin-alpha" => native_a.clone(),
                        "callout-beta" | "dialin-beta" => native_b.clone(),
                        _ => return Err("unknown native fixture port".into()),
                    },
                )
            },
        )
        .unwrap();
        assert_eq!(selected.port.port_name, b.port_name);
        let mut wrong_class = native_a;
        wrong_class.cdc_data_interface = false;
        assert!(verify_native(&a, wrong_class).is_err());
    }

    #[test]
    fn windows_instance_fixture_distinguishes_interface_and_physical_parent() {
        assert_eq!(
            windows_usb_instance(r"USB\VID_1A86&PID_55DE&MI_02\6&ABC&0&0002"),
            Some((CH347_VID, CH347F_PID, Some(2), "6&ABC&0&0002"))
        );
        assert_eq!(
            windows_usb_instance(r"USB\VID_1A86&PID_55DE\test"),
            Some((CH347_VID, CH347F_PID, None, "test"))
        );
        for bad in [
            "COM3",
            r"USB\VID_1A86&PID_55DE&MI_ZZ\x",
            r"USB\VID_1A86&PID_55DE\",
            r"OTHER\VID_1A86&PID_55DE\test",
        ] {
            assert!(windows_usb_instance(bad).is_none());
        }
    }

    fn port(name: &str, pid: u16) -> SerialPortInfo {
        SerialPortInfo {
            port_name: name.to_owned(),
            port_type: SerialPortType::UsbPort(tokio_serial::UsbPortInfo {
                vid: CH347_VID,
                pid,
                serial_number: Some("test".to_owned()),
                manufacturer: None,
                product: None,
                interface: None,
            }),
        }
    }

    fn identity(port: &SerialPortInfo) -> Result<Identity, String> {
        let (device, interface) = port.port_name.split_once(':').unwrap();
        Ok(Identity {
            device: device.to_owned(),
            serial: device.to_owned(),
            interface: interface.parse().unwrap(),
            canonical_port: port.port_name.clone(),
        })
    }

    #[test]
    fn discovery_fails_closed_on_wrong_missing_or_ambiguous_identity() {
        assert!(select(vec![port("wrong:D1", 0x7523)], 0, None, |_| panic!(
            "wrong PID inspected"
        ))
        .is_err());
        let pair = vec![port("a:2", CH347F_PID), port("a:0", CH347F_PID)];
        assert_eq!(
            select(pair.clone(), 0, None, identity)
                .unwrap()
                .port
                .port_name,
            "a:0"
        );
        assert!(select(
            pair.clone(),
            0,
            None,
            |_| Err("missing identity".to_owned())
        )
        .is_err());
        assert!(select(vec![pair[0].clone()], 0, None, identity).is_err());
        assert!(select(vec![pair[0].clone(), pair[0].clone()], 2, None, identity).is_err());
        let mut two = pair;
        two.extend([port("b:0", CH347F_PID), port("b:2", CH347F_PID)]);
        assert!(select(two.clone(), 0, None, identity).is_err());
        assert_eq!(
            select(two.clone(), 2, Some("b"), identity)
                .unwrap()
                .port
                .port_name,
            "b:2"
        );
        assert!(select(two.clone(), 0, Some("missing"), identity).is_err());
        assert!(select(two, 0, Some("duplicate"), |port| {
            let mut id = identity(port)?;
            id.serial = "duplicate".to_owned();
            Ok(id)
        })
        .is_err());
    }
}
