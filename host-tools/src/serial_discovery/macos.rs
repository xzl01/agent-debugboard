//! Read-only IOKit ancestry: serial BSD service -> USB interface -> physical USB node.
use std::{ffi::CStr, ptr};

use core_foundation::{
    base::{CFType, TCFType},
    number::CFNumber,
    string::CFString,
};
use io_kit_sys::{keys::kIOServicePlane, types::io_object_t, *};

use super::{NativeIdentity, SerialPortInfo};

struct Object(io_object_t);
impl Drop for Object {
    fn drop(&mut self) {
        unsafe {
            IOObjectRelease(self.0);
        }
    }
}

impl Object {
    fn conforms(&self, class: &CStr) -> bool {
        // IOKit treats className as an input string (the binding predates const).
        unsafe { IOObjectConformsTo(self.0, class.as_ptr().cast_mut()) != 0 }
    }

    fn property(&self, name: &str) -> Option<CFType> {
        let key = CFString::new(name);
        let value = unsafe {
            IORegistryEntryCreateCFProperty(self.0, key.as_concrete_TypeRef(), ptr::null(), 0)
        };
        if value.is_null() {
            None
        } else {
            // Create rule: the CFType wrapper releases the owned property reference.
            Some(unsafe { CFType::wrap_under_create_rule(value) })
        }
    }

    fn string(&self, name: &str) -> Option<String> {
        self.property(name)?
            .downcast::<CFString>()
            .map(|value| value.to_string())
    }

    fn number(&self, name: &str) -> Option<u16> {
        self.property(name)?
            .downcast::<CFNumber>()?
            .to_i64()
            .and_then(|value| u16::try_from(value).ok())
    }

    fn parent(&self) -> Result<Self, String> {
        let mut parent = 0;
        if unsafe { IORegistryEntryGetParentEntry(self.0, kIOServicePlane, &mut parent) } != 0
            || parent == 0
        {
            return Err("missing IOKit USB parent".into());
        }
        Ok(Self(parent))
    }
}

fn parent_identity(mut node: Object, canonical_port: String) -> Result<NativeIdentity, String> {
    let mut interface = None;
    for _ in 0..32 {
        if node.conforms(c"IOUSBHostInterface") || node.conforms(c"IOUSBInterface") {
            let raw = node
                .number("bInterfaceNumber")
                .and_then(|value| u8::try_from(value).ok())
                .ok_or("missing IOKit USB interface number")?;
            let data = match node.number("bInterfaceClass") {
                Some(0x02) => false,
                Some(0x0a) => true,
                _ => return Err("IOKit interface is not CDC communication/data".into()),
            };
            if interface.replace((raw, data)).is_some() {
                return Err("ambiguous IOKit interface ancestry".into());
            }
        }
        if node.conforms(c"IOUSBHostDevice") || node.conforms(c"IOUSBDevice") {
            let (interface, data) = interface.ok_or("missing IOKit USB interface ancestor")?;
            let mut id = 0;
            if unsafe { IORegistryEntryGetRegistryEntryID(node.0, &mut id) } != 0 || id == 0 {
                return Err("cannot identify physical IOKit USB node".into());
            }
            return Ok(NativeIdentity {
                device: format!("iokit:{id}"),
                serial: node
                    .string("USB Serial Number")
                    .ok_or("missing physical USB serial")?,
                vid: node.number("idVendor").ok_or("missing USB VID")?,
                pid: node.number("idProduct").ok_or("missing USB PID")?,
                interface,
                cdc_data_interface: data,
                canonical_port,
            });
        }
        node = node.parent()?;
    }
    Err("IOKit ancestry exceeds identity bound".into())
}

pub(super) fn native_identity(port: &SerialPortInfo) -> Result<NativeIdentity, String> {
    // Only registry metadata is inspected. IOServiceOpen and serial open are not used.
    let matching = unsafe { IOServiceMatching(c"IOSerialBSDClient".as_ptr()) };
    if matching.is_null() {
        return Err("cannot build IOKit serial match".into());
    }
    let mut iterator = 0;
    // IOServiceGetMatchingServices consumes the matching dictionary reference.
    if unsafe { IOServiceGetMatchingServices(kIOMasterPortDefault, matching, &mut iterator) } != 0
        || iterator == 0
    {
        return Err("cannot enumerate IOKit serial services".into());
    }
    let iterator = Object(iterator);
    let mut identity = None;
    loop {
        let raw = unsafe { IOIteratorNext(iterator.0) };
        if raw == 0 {
            break;
        }
        let service = Object(raw);
        let callout = service.string("IOCalloutDevice");
        let dialin = service.string("IODialinDevice");
        if callout.as_deref() == Some(port.port_name.as_str())
            || dialin.as_deref() == Some(port.port_name.as_str())
        {
            if identity.is_some() {
                return Err("ambiguous IOKit serial service".into());
            }
            identity = Some(parent_identity(
                service,
                callout.ok_or("missing IOKit callout path")?,
            )?);
        }
    }
    if unsafe { IOIteratorIsValid(iterator.0) } == 0 {
        return Err("IOKit registry changed during enumeration".into());
    }
    identity.ok_or_else(|| format!("no native USB identity for {}", port.port_name))
}
