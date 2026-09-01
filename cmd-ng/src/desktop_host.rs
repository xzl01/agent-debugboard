use super::APP_DATA_DIR_NAME;
use anyhow::{bail, Context, Result};
use std::{
    fs,
    io::{Read, Write},
    net::{SocketAddr, TcpStream},
    path::Path,
    process::Child,
    thread,
    time::{Duration, Instant},
};

const HOST_READY_TIMEOUT: Duration = Duration::from_secs(5);
const HOST_READY_POLL: Duration = Duration::from_millis(100);

pub(super) struct HostLaunch<'a> {
    pub(super) port: u16,
    pub(super) previous_host_pid: Option<u64>,
    pub(super) launcher_name: &'a str,
    pub(super) log_path: Option<&'a Path>,
    pub(super) tray_lock: Option<&'a Path>,
    pub(super) replacing_headless: bool,
}

impl HostLaunch<'_> {
    pub(super) fn accepts_host(&self, current_pid: u64, tray_ready: bool) -> bool {
        self.previous_host_pid
            .is_none_or(|previous| current_pid != previous)
            || (!self.replacing_headless && tray_ready)
    }
}

fn parse_tray_lock(owner: &str) -> Option<(u32, &str)> {
    let owner = owner.strip_suffix('\n')?;
    let (pid_text, mode) = owner.split_once(' ')?;
    if pid_text.is_empty() || !pid_text.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    let pid = pid_text.parse::<u32>().ok()?;
    (pid > 0 && pid.to_string() == pid_text).then_some((pid, mode))
}

pub(super) fn tray_lock_owner_is_headless(owner: &str, host_pid: Option<u64>) -> bool {
    parse_tray_lock(owner).is_some_and(|(owner_pid, mode)| {
        mode == "headless" && host_pid == Some(u64::from(owner_pid))
    })
}

pub(super) fn tray_lock_allows_ready(
    owner: &str,
    launcher_pid: u32,
    launcher_running: bool,
) -> bool {
    let Some((owner_pid, mode)) = parse_tray_lock(owner) else {
        return false;
    };
    if mode != "tray" {
        return false;
    }
    if launcher_running {
        owner_pid == launcher_pid
    } else {
        owner_pid != launcher_pid
    }
}

pub(super) fn host_not_ready_message(port: u16, log_path: Option<&Path>) -> String {
    let log_path = log_path.map_or_else(
        || format!("<per-user local data>/{APP_DATA_DIR_NAME}/host.log"),
        |path| path.display().to_string(),
    );
    format!("Linkr Host did not become ready at 127.0.0.1:{port}; Host log: {log_path}")
}

pub(super) fn host_pid(port: u16) -> Option<u64> {
    let address = SocketAddr::from(([127, 0, 0, 1], port));
    let mut stream = TcpStream::connect_timeout(&address, Duration::from_millis(300)).ok()?;
    stream
        .set_read_timeout(Some(Duration::from_millis(500)))
        .ok()?;
    stream
        .write_all(
            format!(
                "GET /host/api/v1/status HTTP/1.1\r\nHost: {address}\r\nConnection: close\r\n\r\n"
            )
            .as_bytes(),
        )
        .ok()?;
    let mut response = String::new();
    stream.read_to_string(&mut response).ok()?;
    let (headers, body) = response.split_once("\r\n\r\n")?;
    if !headers.starts_with("HTTP/1.1 200") {
        return None;
    }
    let status = serde_json::from_str::<serde_json::Value>(body).ok()?;
    if status.get("ok").and_then(serde_json::Value::as_bool) != Some(true)
        || status.get("schema").and_then(serde_json::Value::as_str) != Some("radxa-linkr-host.v1")
    {
        return None;
    }
    status.get("pid").and_then(serde_json::Value::as_u64)
}

pub(super) fn host_is_ready(port: u16) -> bool {
    host_pid(port).is_some()
}

pub(super) fn wait_for_host_ready(target: HostLaunch<'_>, launcher: &mut Child) -> Result<()> {
    let deadline = Instant::now() + HOST_READY_TIMEOUT;
    let mut launcher_running = true;
    loop {
        if launcher_running {
            if let Some(status) = launcher
                .try_wait()
                .with_context(|| format!("inspect {} startup", target.launcher_name))?
            {
                launcher_running = false;
                if !status.success() {
                    bail!(
                        "{} exited with {status} before Linkr Host became ready; {}",
                        target.launcher_name,
                        host_not_ready_message(target.port, target.log_path)
                    );
                }
            }
        }
        if let Some(pid) = host_pid(target.port) {
            let tray_ready = target
                .tray_lock
                .and_then(|path| fs::read_to_string(path).ok())
                .is_some_and(|owner| {
                    tray_lock_allows_ready(&owner, launcher.id(), launcher_running)
                });
            if target.accepts_host(pid, tray_ready) {
                return Ok(());
            }
        }
        if Instant::now() >= deadline {
            bail!("{}", host_not_ready_message(target.port, target.log_path));
        }
        thread::sleep(HOST_READY_POLL);
    }
}
