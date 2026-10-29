//! Read-only SetupAPI / Configuration Manager identity. Never opens a COM handle.
use std::{
    mem::{size_of, size_of_val},
    ptr,
};

use windows_sys::Win32::{
    Devices::DeviceAndDriverInstallation::*,
    Foundation::{GetLastError, ERROR_NO_MORE_ITEMS, ERROR_SUCCESS, INVALID_HANDLE_VALUE},
    System::Registry::{RegCloseKey, RegQueryValueExW, KEY_READ, REG_SZ},
};

use super::{windows_usb_instance, NativeIdentity, SerialPortInfo, CH347F_PID, CH347_VID};

struct DeviceSet(HDEVINFO);
impl Drop for DeviceSet {
    fn drop(&mut self) {
        unsafe {
            SetupDiDestroyDeviceInfoList(self.0);
        }
    }
}

fn instance_id(node: u32) -> Result<String, String> {
    let mut text = [0u16; MAX_DEVICE_ID_LEN as usize];
    // SAFETY: fixed writable buffer, valid devnode obtained from SetupAPI/CM.
    if unsafe { CM_Get_Device_IDW(node, text.as_mut_ptr(), text.len() as u32, 0) } != CR_SUCCESS {
        return Err("cannot read USB device instance ID".into());
    }
    let end = text
        .iter()
        .position(|value| *value == 0)
        .ok_or("unterminated instance ID")?;
    String::from_utf16(&text[..end]).map_err(|error| error.to_string())
}

fn port_name(set: HDEVINFO, data: &SP_DEVINFO_DATA) -> Option<String> {
    // SAFETY: data belongs to set; the registry handle is read-only and always closed.
    let key = unsafe { SetupDiOpenDevRegKey(set, data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ) };
    if key == INVALID_HANDLE_VALUE {
        return None;
    }
    let mut text = [0u16; 256];
    let mut bytes = size_of::<[u16; 256]>() as u32;
    let mut kind = 0;
    let name: Vec<u16> = "PortName\0".encode_utf16().collect();
    let result = unsafe {
        RegQueryValueExW(
            key,
            name.as_ptr(),
            ptr::null(),
            &mut kind,
            text.as_mut_ptr().cast(),
            &mut bytes,
        )
    };
    unsafe {
        RegCloseKey(key);
    }
    if result != ERROR_SUCCESS
        || kind != REG_SZ
        || bytes as usize > size_of_val(&text)
        || bytes % 2 != 0
    {
        return None;
    }
    let text = &text[..bytes as usize / 2];
    let end = text.iter().position(|value| *value == 0)?;
    String::from_utf16(&text[..end]).ok()
}

fn parent_identity(mut node: u32, port: &SerialPortInfo) -> Result<NativeIdentity, String> {
    let mut interface = None;
    for _ in 0..32 {
        let id = instance_id(node)?;
        if let Some((vid, pid, mi, serial)) = windows_usb_instance(&id) {
            if vid != CH347_VID || pid != CH347F_PID {
                return Err("COM port belongs to a different USB device".into());
            }
            if let Some(mi) = mi {
                if interface.is_some_and(|previous| previous != mi) {
                    return Err("ambiguous USB interface ancestry".into());
                }
                interface = Some(mi);
            } else {
                return Ok(NativeIdentity {
                    // DEVINST is the physical parent node, not a serial-string grouping.
                    device: format!("windows:{node}:{id}"),
                    serial: serial.to_owned(),
                    vid,
                    pid,
                    interface: interface.ok_or("missing USB interface ancestor")?,
                    cdc_data_interface: false,
                    canonical_port: port.port_name.clone(),
                });
            }
        }
        let mut parent = 0;
        if unsafe { CM_Get_Parent(&mut parent, node, 0) } != CR_SUCCESS {
            return Err("missing physical USB parent".into());
        }
        node = parent;
    }
    Err("USB ancestry exceeds identity bound".into())
}

pub(super) fn native_identity(port: &SerialPortInfo) -> Result<NativeIdentity, String> {
    // SAFETY: constant class GUID, null optional inputs; no device handles opened.
    let raw = unsafe {
        SetupDiGetClassDevsW(
            &GUID_DEVCLASS_PORTS,
            ptr::null(),
            ptr::null_mut(),
            DIGCF_PRESENT,
        )
    };
    if raw == -1 {
        return Err("cannot enumerate native COM identities".into());
    }
    let set = DeviceSet(raw);
    let mut identity = None;
    for index in 0.. {
        let mut data: SP_DEVINFO_DATA = unsafe { std::mem::zeroed() };
        data.cbSize = size_of::<SP_DEVINFO_DATA>() as u32;
        if unsafe { SetupDiEnumDeviceInfo(set.0, index, &mut data) } == 0 {
            if unsafe { GetLastError() } == ERROR_NO_MORE_ITEMS {
                break;
            }
            return Err("native COM enumeration failed".into());
        }
        if port_name(set.0, &data).is_some_and(|name| name.eq_ignore_ascii_case(&port.port_name)) {
            if identity.is_some() {
                return Err("ambiguous native COM identity".into());
            }
            identity = Some(parent_identity(data.DevInst, port)?);
        }
    }
    identity.ok_or_else(|| format!("no native USB identity for {}", port.port_name))
}
