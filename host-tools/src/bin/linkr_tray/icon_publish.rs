use anyhow::{Context, Result};
use std::path::Path;
use tray_icon::menu::Menu;

#[cfg(not(target_os = "linux"))]
use tray_icon::{Icon, TrayIcon, TrayIconBuilder};
#[cfg(target_os = "linux")]
use {
    anyhow::ensure,
    libappindicator::{AppIndicator, AppIndicatorStatus},
    std::{
        cell::RefCell,
        fs::{self, DirBuilder, OpenOptions},
        io::{BufWriter, Write},
        os::unix::fs::{DirBuilderExt, OpenOptionsExt},
        path::PathBuf,
        rc::Rc,
    },
    tray_icon::menu::ContextMenu,
};

const ICON_SIDE: u32 = 32;

pub(super) struct IconPublisher {
    #[cfg(target_os = "linux")]
    assets: Rc<IconAssets>,
}

pub(super) struct TrayHandle {
    #[cfg(target_os = "linux")]
    indicator: RefCell<AppIndicator>,
    #[cfg(target_os = "linux")]
    _menu: Menu,
    // Retain the files even if the publisher is dropped before the indicator.
    #[cfg(target_os = "linux")]
    assets: Rc<IconAssets>,
    #[cfg(not(target_os = "linux"))]
    tray: TrayIcon,
}

impl IconPublisher {
    pub(super) fn new(data_dir: &Path) -> Result<Self> {
        #[cfg(target_os = "linux")]
        {
            Ok(Self {
                assets: Rc::new(IconAssets::new(data_dir)?),
            })
        }
        #[cfg(not(target_os = "linux"))]
        {
            let _ = data_dir;
            Ok(Self {})
        }
    }

    pub(super) fn build(
        &self,
        menu: Menu,
        tooltip: &str,
        key: u16,
        rgba: &[u8],
    ) -> Result<TrayHandle> {
        #[cfg(target_os = "linux")]
        {
            let _ = tooltip; // libappindicator has no tooltip support.
            let name = self.assets.publish(key, rgba)?;
            let path = self
                .assets
                .directory
                .to_str()
                .context("tray icon directory is not UTF-8")?;
            // Tao has already initialized GTK. Own exactly one native indicator;
            // only its icon name changes after this initial theme-path setup.
            let mut indicator = AppIndicator::with_path("radxa-linkr", &name, path);
            indicator.set_title("Radxa Linkr");
            indicator.set_menu(&mut menu.gtk_context_menu());
            indicator.set_icon_full(&name, "Radxa Linkr");
            indicator.set_status(AppIndicatorStatus::Active);
            Ok(TrayHandle {
                indicator: RefCell::new(indicator),
                _menu: menu,
                assets: Rc::clone(&self.assets),
            })
        }
        #[cfg(not(target_os = "linux"))]
        {
            let _ = key;
            let tray = TrayIconBuilder::new()
                .with_menu(Box::new(menu))
                .with_tooltip(tooltip)
                .with_icon(Icon::from_rgba(rgba.to_vec(), ICON_SIDE, ICON_SIDE)?)
                .build()
                .context("create tray icon")?;
            Ok(TrayHandle { tray })
        }
    }

    pub(super) fn set_icon(&self, tray: &TrayHandle, key: u16, rgba: &[u8]) -> Result<()> {
        #[cfg(target_os = "linux")]
        {
            ensure!(
                Rc::ptr_eq(&self.assets, &tray.assets),
                "tray belongs to a different icon publisher"
            );
            let name = self.assets.publish(key, rgba)?;
            tray.indicator
                .borrow_mut()
                .set_icon_full(&name, "Radxa Linkr");
            Ok(())
        }
        #[cfg(not(target_os = "linux"))]
        {
            let _ = key;
            tray.tray
                .set_icon(Some(Icon::from_rgba(rgba.to_vec(), ICON_SIDE, ICON_SIDE)?))
                .map_err(Into::into)
        }
    }
}

impl TrayHandle {
    pub(super) fn set_tooltip(&self, tooltip: Option<&str>) -> Result<()> {
        #[cfg(target_os = "linux")]
        {
            let _ = tooltip;
            Ok(())
        }
        #[cfg(not(target_os = "linux"))]
        self.tray.set_tooltip(tooltip).map_err(Into::into)
    }
}

#[cfg(target_os = "linux")]
impl Drop for TrayHandle {
    fn drop(&mut self) {
        // Withdraw the indicator before releasing its menu and owned PNGs.
        self.indicator
            .get_mut()
            .set_status(AppIndicatorStatus::Passive);
    }
}

#[cfg(target_os = "linux")]
const MAX_ICON_KEY: u16 = 0x1ff;
#[cfg(target_os = "linux")]
const ICON_RGBA_LEN: usize = ICON_SIDE as usize * ICON_SIDE as usize * 4;

#[cfg(target_os = "linux")]
struct IconAssets {
    directory: PathBuf,
    published: RefCell<[bool; MAX_ICON_KEY as usize + 1]>,
}

#[cfg(target_os = "linux")]
impl IconAssets {
    fn new(data_dir: &Path) -> Result<Self> {
        fs::create_dir_all(data_dir)
            .with_context(|| format!("create tray data directory {}", data_dir.display()))?;
        let directory = data_dir.canonicalize()?.join(format!(
            "tray-icons-{}-{}",
            std::process::id(),
            uuid::Uuid::new_v4()
        ));
        DirBuilder::new()
            .mode(0o700)
            .create(&directory)
            .with_context(|| {
                format!("create private tray icon directory {}", directory.display())
            })?;
        Ok(Self {
            directory,
            published: RefCell::new([false; MAX_ICON_KEY as usize + 1]),
        })
    }

    fn icon_name(key: u16) -> Result<String> {
        ensure!(key <= MAX_ICON_KEY, "tray icon key out of range: {key:#x}");
        Ok(format!("linkr-state-{key:03x}"))
    }

    fn publish(&self, key: u16, rgba: &[u8]) -> Result<String> {
        let name = Self::icon_name(key)?;
        ensure!(
            rgba.len() == ICON_RGBA_LEN,
            "tray icon must contain 32x32 RGBA8 pixels"
        );
        let mut published = self.published.borrow_mut();
        if !published[key as usize] {
            self.write_png(&name, rgba)?;
            published[key as usize] = true;
        }
        // A key's RGBA is constant for this process. Reusing its immutable PNG
        // lets the desktop keep both heartbeat images warm in its icon cache.
        Ok(name)
    }

    fn write_png(&self, name: &str, rgba: &[u8]) -> Result<()> {
        let temporary_path = self.directory.join(format!(".{name}.png.tmp"));
        let file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&temporary_path)
            .context("create temporary tray PNG")?;
        let temporary = TemporaryIcon(temporary_path);
        {
            let mut output = BufWriter::new(file);
            let mut encoder = png::Encoder::new(&mut output, ICON_SIDE, ICON_SIDE);
            encoder.set_color(png::ColorType::Rgba);
            encoder.set_depth(png::BitDepth::Eight);
            let mut writer = encoder.write_header().context("write tray PNG header")?;
            writer
                .write_image_data(rgba)
                .context("write tray PNG pixels")?;
            writer.finish().context("finish tray PNG")?;
            output.flush().context("flush tray PNG")?;
        }
        fs::rename(&temporary.0, self.directory.join(format!("{name}.png")))
            .context("publish complete tray PNG")?;
        Ok(())
    }
}

#[cfg(target_os = "linux")]
impl Drop for IconAssets {
    fn drop(&mut self) {
        // This is only the unique directory we created, never an old/shared
        // tray-icons directory or another process's cached icons.
        let _ = fs::remove_dir_all(&self.directory);
    }
}

#[cfg(target_os = "linux")]
struct TemporaryIcon(PathBuf);

#[cfg(target_os = "linux")]
impl Drop for TemporaryIcon {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}

#[cfg(all(test, target_os = "linux"))]
#[path = "icon_publish_test.rs"]
mod tests;
