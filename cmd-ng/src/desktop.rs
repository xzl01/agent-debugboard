use anyhow::{bail, Context, Result};
use std::env;
#[cfg(any(target_os = "linux", test))]
use std::ffi::OsStr;
use std::fs::{create_dir_all, OpenOptions};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};

#[path = "desktop_host.rs"]
mod host;
use host::{host_is_ready, host_pid, wait_for_host_ready, HostLaunch};

const DEFAULT_HOST_PORT: u16 = 8787;
const APP_DATA_DIR_NAME: &str = "radxa-linkr-debugger";
#[cfg(windows)]
const TRAY_TOOL_NAME: &str = "linkr-tray.exe";
#[cfg(not(windows))]
const TRAY_TOOL_NAME: &str = "linkr-tray";

pub fn ensure_desktop() -> Result<()> {
    if env::var_os("LINKR_SKIP_DESKTOP").is_some() {
        return Ok(());
    }
    let port = host_port();
    let log_path = host_log_path();
    let tray_lock = tray_lock_path();
    let previous_host_pid = host_pid(port);
    let replacing_headless = tray_lock
        .as_deref()
        .and_then(|path| std::fs::read_to_string(path).ok())
        .is_some_and(|owner| host::tray_lock_owner_is_headless(&owner, previous_host_pid));
    let graphical = graphical_session_available();
    if !should_launch_tray(previous_host_pid.is_some(), graphical) {
        return Ok(());
    }
    if !graphical {
        return ensure_tray_daemon(port, true, log_path.as_deref());
    }

    let tray_error = match find_tray_binary()
        .and_then(|tray| spawn_detached(tray_command(&tray, port, false), log_path.as_deref()))
    {
        Ok(mut launcher) => {
            if let Err(error) = wait_for_host_ready(
                HostLaunch {
                    port,
                    previous_host_pid,
                    launcher_name: "linkr-tray",
                    log_path: log_path.as_deref(),
                    tray_lock: tray_lock.as_deref(),
                    replacing_headless,
                },
                &mut launcher,
            ) {
                if launcher
                    .try_wait()
                    .context("inspect linkr-tray before headless fallback")?
                    .is_none()
                {
                    return Err(error);
                }
                error
            } else {
                return Ok(());
            }
        }
        Err(error) => error,
    };

    ensure_tray_daemon(port, true, log_path.as_deref()).with_context(|| {
        format!("linkr-tray unavailable ({tray_error:#}); start headless desktop daemon")
    })
}

#[cfg(any(target_os = "linux", test))]
fn graphical_session_available_from(
    display: Option<&OsStr>,
    wayland_display: Option<&OsStr>,
) -> bool {
    display.is_some_and(|value| !value.is_empty())
        || wayland_display.is_some_and(|value| !value.is_empty())
}

#[cfg(target_os = "linux")]
fn graphical_session_available() -> bool {
    graphical_session_available_from(
        env::var_os("DISPLAY").as_deref(),
        env::var_os("WAYLAND_DISPLAY").as_deref(),
    )
}

#[cfg(not(target_os = "linux"))]
fn graphical_session_available() -> bool {
    true
}

fn ensure_tray_daemon(port: u16, headless: bool, log_path: Option<&Path>) -> Result<()> {
    if host_is_ready(port) {
        return Ok(());
    }
    let tray = find_tray_binary()?;
    let mut launcher = spawn_detached(tray_command(&tray, port, headless), log_path)?;
    wait_for_host_ready(
        HostLaunch {
            port,
            previous_host_pid: None,
            launcher_name: "linkr-tray",
            log_path,
            tray_lock: None,
            replacing_headless: false,
        },
        &mut launcher,
    )
}

fn host_port() -> u16 {
    env::var("LINKR_BRIDGE_PORT")
        .ok()
        .and_then(|value| value.parse().ok())
        .unwrap_or(DEFAULT_HOST_PORT)
}

fn host_log_path() -> Option<PathBuf> {
    #[cfg(target_os = "windows")]
    let data_dir = env::var_os("LOCALAPPDATA").map(PathBuf::from);
    #[cfg(target_os = "macos")]
    let data_dir = env::var_os("HOME")
        .map(PathBuf::from)
        .map(|path| path.join("Library").join("Application Support"));
    #[cfg(all(unix, not(target_os = "macos")))]
    let data_dir = env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .filter(|path| path.is_absolute())
        .or_else(|| {
            env::var_os("HOME")
                .map(PathBuf::from)
                .map(|path| path.join(".local").join("share"))
        });
    #[cfg(not(any(target_os = "windows", target_os = "macos", unix)))]
    let data_dir: Option<PathBuf> = None;

    data_dir.map(|path| path.join(APP_DATA_DIR_NAME).join("host.log"))
}

fn tray_lock_path() -> Option<PathBuf> {
    host_log_path().and_then(|path| path.parent().map(|directory| directory.join("tray.lock")))
}

const fn should_launch_tray(host_ready: bool, graphical: bool) -> bool {
    graphical || !host_ready
}

fn find_tray_binary() -> Result<PathBuf> {
    find_tool_binary(TRAY_TOOL_NAME, "LINKR_TRAY_BIN")
}

fn find_tool_binary(name: &str, override_variable: &str) -> Result<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(path) = env::var_os(override_variable) {
        candidates.push(PathBuf::from(path));
    }
    #[cfg(debug_assertions)]
    {
        let manifest = Path::new(env!("CARGO_MANIFEST_DIR"));
        candidates.push(manifest.join("../host-tools/target/debug").join(name));
        candidates.push(manifest.join("../host-tools/target/release").join(name));
    }
    if let Ok(exe) = env::current_exe() {
        if let Some(dir) = exe.parent() {
            candidates.push(dir.join(name));
            #[cfg(windows)]
            candidates.push(dir.join(name.trim_end_matches(".exe")));
        }
    }
    #[cfg(unix)]
    if let Some(home) = env::var_os("HOME") {
        let home = PathBuf::from(home);
        #[cfg(target_os = "macos")]
        candidates.push(
            home.join("Library")
                .join("Application Support")
                .join("Radxa Linkr Debugger")
                .join("bin")
                .join(name),
        );
        #[cfg(all(unix, not(target_os = "macos")))]
        {
            if let Some(data) = env::var_os("XDG_DATA_HOME") {
                candidates.push(
                    PathBuf::from(data)
                        .join("radxa-linkr-debugger")
                        .join("bin")
                        .join(name),
                );
            }
            candidates.push(
                home.join(".local")
                    .join("share")
                    .join("radxa-linkr-debugger")
                    .join("bin")
                    .join(name),
            );
        }
    }
    if let Some(path) = find_on_path(name) {
        candidates.push(path);
    }
    if let Some(path) = candidates.into_iter().find(|path| path.is_file()) {
        return Ok(path);
    }
    bail!("{name} not found; install the Radxa Linkr desktop stack or set {override_variable}")
}

fn find_on_path(name: &str) -> Option<PathBuf> {
    let path = env::var_os("PATH")?;
    env::split_paths(&path)
        .map(|entry| entry.join(name))
        .find(|entry| entry.is_file())
}

fn configure_desktop_output(command: &mut Command, log_path: Option<&Path>) -> Result<()> {
    if let Some(log_path) = log_path {
        if let Some(parent) = log_path.parent() {
            create_dir_all(parent)
                .with_context(|| format!("create desktop log directory {}", parent.display()))?;
        }
        let log = OpenOptions::new()
            .create(true)
            .append(true)
            .open(log_path)
            .with_context(|| format!("open desktop log {}", log_path.display()))?;
        command
            .stdout(Stdio::from(log.try_clone()?))
            .stderr(Stdio::from(log));
    } else {
        command.stdout(Stdio::null()).stderr(Stdio::null());
    }
    Ok(())
}

fn tray_command(path: &Path, port: u16, headless: bool) -> Command {
    #[cfg(target_os = "linux")]
    let mut command = {
        let mut command = Command::new("setsid");
        command.arg(path);
        command
    };
    #[cfg(not(target_os = "linux"))]
    let mut command = Command::new(path);
    command
        .arg("--no-open-when-running")
        .arg("--port")
        .arg(port.to_string());
    if headless {
        command.arg("--headless");
    }
    command
}

fn spawn_detached(mut command: Command, log_path: Option<&Path>) -> Result<Child> {
    command.stdin(Stdio::null());
    configure_desktop_output(&mut command, log_path)?;
    #[cfg(all(unix, not(target_os = "linux")))]
    {
        use std::os::unix::process::CommandExt;
        command.process_group(0);
    }
    let program = command.get_program().to_string_lossy().into_owned();
    command
        .spawn()
        .with_context(|| format!("failed to start {program}"))
}

#[cfg(test)]
#[path = "desktop_test.rs"]
mod tests;
