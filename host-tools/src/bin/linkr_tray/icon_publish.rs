use anyhow::{Context, Result};
use std::path::{Path, PathBuf};
use tray_icon::{Icon, TrayIcon, TrayIconBuilder};

#[cfg(target_os = "linux")]
const ICON_SLOT_COUNT: usize = 4;

pub(super) struct IconPublisher {
    #[cfg(target_os = "linux")]
    slots: [PathBuf; ICON_SLOT_COUNT],
    #[cfg(target_os = "linux")]
    current: usize,
}

impl IconPublisher {
    pub(super) fn new(data_dir: &Path) -> Result<Self> {
        #[cfg(target_os = "linux")]
        {
            let root = data_dir.join("tray-icons");
            // ponytail: four slots retain 240 ms at the 60 ms Logic cadence;
            // increase only if measured D-Bus propagation exceeds that bound.
            let slots = std::array::from_fn(|index| root.join(index.to_string()));
            for slot in &slots {
                std::fs::create_dir_all(slot)
                    .with_context(|| format!("create tray icon slot {}", slot.display()))?;
                set_slot_writable(slot, true)?;
                clear_slot(slot)?;
            }
            Ok(Self { slots, current: 0 })
        }
        #[cfg(not(target_os = "linux"))]
        {
            let _ = data_dir;
            Ok(Self {})
        }
    }

    pub(super) fn configure(&self, builder: TrayIconBuilder) -> TrayIconBuilder {
        #[cfg(target_os = "linux")]
        {
            builder.with_temp_dir_path(&self.slots[self.current])
        }
        #[cfg(not(target_os = "linux"))]
        builder
    }

    pub(super) fn protect_initial(&self) -> Result<()> {
        #[cfg(target_os = "linux")]
        set_slot_writable(&self.slots[self.current], false)?;
        Ok(())
    }

    pub(super) fn set_icon(&mut self, tray: &TrayIcon, icon: Icon) -> Result<()> {
        #[cfg(target_os = "linux")]
        {
            let previous = self.current;
            let next = (previous + 1) % ICON_SLOT_COUNT;
            set_slot_writable(&self.slots[next], true)?;
            clear_slot(&self.slots[next])?;
            tray.set_temp_dir_path(Some(&self.slots[next]));
            tray.set_icon(Some(icon))?;
            set_slot_writable(&self.slots[next], false)?;
            self.current = next;
            set_slot_writable(&self.slots[previous], true)?;
            Ok(())
        }
        #[cfg(not(target_os = "linux"))]
        tray.set_icon(Some(icon)).map_err(Into::into)
    }
}

#[cfg(target_os = "linux")]
fn set_slot_writable(path: &Path, writable: bool) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;

    let mode = if writable { 0o700 } else { 0o500 };
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(mode))
        .with_context(|| format!("set tray icon slot permissions for {}", path.display()))
}

#[cfg(target_os = "linux")]
fn clear_slot(path: &Path) -> Result<()> {
    for entry in std::fs::read_dir(path)
        .with_context(|| format!("read tray icon slot {}", path.display()))?
    {
        let entry = entry?;
        std::fs::remove_file(entry.path())
            .with_context(|| format!("clear tray icon file {}", entry.path().display()))?;
    }
    Ok(())
}

#[cfg(all(test, target_os = "linux"))]
#[path = "icon_publish_test.rs"]
mod tests;
