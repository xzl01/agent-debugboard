use std::{
    fs::{File, OpenOptions},
    io::{Read, Write},
    net::{IpAddr, Ipv4Addr, SocketAddr, TcpStream},
    path::PathBuf,
    process::{Child, Command, Stdio},
    time::{Duration, Instant},
};

use anyhow::{bail, Context, Result};
use clap::Parser;
use fs2::FileExt;
use serde_json::Value;
use tao::{
    event::Event,
    event_loop::{ControlFlow, EventLoopBuilder},
};
use tray_icon::{
    menu::{Menu, MenuEvent, MenuItem, PredefinedMenuItem},
    Icon, TrayIcon, TrayIconBuilder,
};
use url::Url;
use uuid::Uuid;

use radxa_linkr_host::config::DEFAULT_BOARD_URL;

const HEALTH_INTERVAL: Duration = Duration::from_secs(2);
const HEALTH_TIMEOUT: Duration = Duration::from_millis(500);
const RESTART_INITIAL: Duration = Duration::from_millis(500);
const RESTART_MAX: Duration = Duration::from_secs(30);
const GATEWAY_SCHEMA: &str = "radxa-linkr-debugger-gateway.v1";

#[derive(Debug, Parser)]
#[command(
    name = "linkr-tray",
    version,
    about = "Tray supervisor for Radxa Linkr Debugger Web, Broker and MCP services"
)]
struct Cli {
    #[arg(long, default_value_t = 8787)]
    port: u16,

    #[arg(long)]
    web_root: Option<PathBuf>,

    #[arg(long, default_value = DEFAULT_BOARD_URL)]
    board_url: Url,

    #[arg(long)]
    host_binary: Option<PathBuf>,

    /// Root directory for automatically archived raw UART RX sessions.
    #[arg(long)]
    serial_log_dir: Option<PathBuf>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ServiceState {
    Starting,
    Ready,
    Offline,
}

#[derive(Debug)]
enum UserEvent {
    Menu(MenuEvent),
    Shutdown,
}

struct MenuItems {
    status: MenuItem,
    open_web: MenuItem,
    open_status: MenuItem,
    open_serial_logs: MenuItem,
    restart: MenuItem,
    quit: MenuItem,
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let data_dir = app_data_dir()?;
    std::fs::create_dir_all(&data_dir)
        .with_context(|| format!("create tray data directory {}", data_dir.display()))?;
    let mut lock = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(data_dir.join("tray.lock"))
        .context("open tray instance lock")?;
    if lock.try_lock_exclusive().is_err() {
        open_browser(&format!("http://127.0.0.1:{}/", cli.port))?;
        return Ok(());
    }
    lock.set_len(0).context("reset tray instance lock")?;
    writeln!(&mut lock, "{}", std::process::id()).context("write tray process id")?;
    lock.sync_data().context("persist tray process id")?;
    let shutdown_request = data_dir.join("shutdown.request");
    let _ = std::fs::remove_file(&shutdown_request);

    let host_binary = cli
        .host_binary
        .clone()
        .unwrap_or(sibling_binary("linkr-host")?);
    let event_loop = EventLoopBuilder::<UserEvent>::with_user_event().build();
    #[cfg(target_os = "macos")]
    let mut event_loop = event_loop;
    #[cfg(target_os = "macos")]
    {
        use tao::platform::macos::{ActivationPolicy, EventLoopExtMacOS};
        event_loop.set_activation_policy(ActivationPolicy::Accessory);
        event_loop.set_dock_visibility(false);
    }

    let menu = Menu::new();
    let items = MenuItems {
        status: MenuItem::new("Starting Web / Broker / MCP…", false, None),
        open_web: MenuItem::new("Open Web Console", true, None),
        open_status: MenuItem::new("Open Service Status", true, None),
        open_serial_logs: MenuItem::new("Open UART Archives", true, None),
        restart: MenuItem::new("Restart Managed Services", false, None),
        quit: MenuItem::new("Quit Radxa Linkr", true, None),
    };
    menu.append_items(&[
        &items.status,
        &PredefinedMenuItem::separator(),
        &items.open_web,
        &items.open_status,
        &items.open_serial_logs,
        &items.restart,
        &PredefinedMenuItem::separator(),
        &items.quit,
    ])
    .context("build tray menu")?;
    let tray = TrayIconBuilder::new()
        .with_menu(Box::new(menu))
        .with_tooltip("Radxa Linkr: starting Web / Broker / MCP")
        .with_icon(status_icon(ServiceState::Starting)?)
        .build()
        .context("create tray icon")?;

    let proxy = event_loop.create_proxy();
    MenuEvent::set_event_handler(Some(move |event| {
        let _ = proxy.send_event(UserEvent::Menu(event));
    }));
    let shutdown_proxy = event_loop.create_proxy();
    ctrlc::set_handler(move || {
        let _ = shutdown_proxy.send_event(UserEvent::Shutdown);
    })
    .context("install tray shutdown handler")?;

    let web_url = format!("http://127.0.0.1:{}/", cli.port);
    let status_url = format!("http://127.0.0.1:{}/host/api/v1/status", cli.port);
    let serial_logs_url = format!("http://127.0.0.1:{}/#serial-logs", cli.port);
    let health_addr = SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), cli.port);
    let shutdown_token = Uuid::new_v4().to_string();
    let mut child: Option<Child> = None;
    let mut last_state = ServiceState::Starting;
    let mut next_health = Instant::now();
    let mut next_restart = Instant::now();
    let mut restart_delay = RESTART_INITIAL;
    let mut shutting_down = false;
    let mut log = open_log(&data_dir)?;

    event_loop.run(move |event, _, control_flow| {
        *control_flow = ControlFlow::WaitUntil(next_health);
        match event {
            Event::UserEvent(UserEvent::Menu(event)) if event.id == *items.open_web.id() => {
                let _ = open_browser(&web_url);
            }
            Event::UserEvent(UserEvent::Menu(event)) if event.id == *items.open_status.id() => {
                let _ = open_browser(&status_url);
            }
            Event::UserEvent(UserEvent::Menu(event))
                if event.id == *items.open_serial_logs.id() =>
            {
                let _ = open_browser(&serial_logs_url);
            }
            Event::UserEvent(UserEvent::Menu(event)) if event.id == *items.restart.id() => {
                if let Some(mut managed) = child.take() {
                    stop_managed_host(&mut managed, health_addr, &shutdown_token, &mut log);
                }
                next_restart = Instant::now();
                restart_delay = RESTART_INITIAL;
                update_tray(&tray, &items, ServiceState::Starting, false);
                last_state = ServiceState::Starting;
            }
            Event::UserEvent(UserEvent::Menu(event)) if event.id == *items.quit.id() => {
                shutting_down = true;
                if let Some(mut managed) = child.take() {
                    stop_managed_host(&mut managed, health_addr, &shutdown_token, &mut log);
                }
                *control_flow = ControlFlow::Exit;
            }
            Event::UserEvent(UserEvent::Shutdown) => {
                shutting_down = true;
                if let Some(mut managed) = child.take() {
                    stop_managed_host(&mut managed, health_addr, &shutdown_token, &mut log);
                }
                *control_flow = ControlFlow::Exit;
            }
            Event::MainEventsCleared if shutdown_request.is_file() => {
                shutting_down = true;
                let _ = std::fs::remove_file(&shutdown_request);
                if let Some(mut managed) = child.take() {
                    stop_managed_host(&mut managed, health_addr, &shutdown_token, &mut log);
                }
                *control_flow = ControlFlow::Exit;
            }
            Event::MainEventsCleared if !shutting_down && Instant::now() >= next_health => {
                if child
                    .as_mut()
                    .is_some_and(|managed| managed.try_wait().ok().flatten().is_some())
                {
                    child = None;
                }

                let healthy = host_is_healthy(health_addr);
                let state = if healthy {
                    restart_delay = RESTART_INITIAL;
                    ServiceState::Ready
                } else {
                    if child.is_none() && Instant::now() >= next_restart {
                        match spawn_host(&host_binary, &cli, &log, &shutdown_token) {
                            Ok(managed) => {
                                child = Some(managed);
                                next_restart = Instant::now() + restart_delay;
                                restart_delay = restart_delay.saturating_mul(2).min(RESTART_MAX);
                            }
                            Err(error) => {
                                let _ = writeln!(log, "failed to start Linkr Host: {error:#}");
                                next_restart = Instant::now() + restart_delay;
                                restart_delay = restart_delay.saturating_mul(2).min(RESTART_MAX);
                            }
                        }
                    }
                    if child.is_some() {
                        ServiceState::Starting
                    } else {
                        ServiceState::Offline
                    }
                };
                if state != last_state {
                    update_tray(&tray, &items, state, child.is_some());
                    last_state = state;
                } else {
                    items.restart.set_enabled(child.is_some());
                }
                next_health = Instant::now() + HEALTH_INTERVAL;
                *control_flow = ControlFlow::WaitUntil(next_health);
            }
            _ => {}
        }
    });
}

fn spawn_host(binary: &PathBuf, cli: &Cli, log: &File, shutdown_token: &str) -> Result<Child> {
    if !binary.is_file() {
        bail!("Linkr Host binary not found at {}", binary.display());
    }
    let mut command = Command::new(binary);
    command
        .arg("serve")
        .arg("--port")
        .arg(cli.port.to_string())
        .arg("--board-url")
        .arg(cli.board_url.as_str())
        .arg("--serial-log-mode")
        .arg("rx")
        .env("LINKR_HOST_SHUTDOWN_TOKEN", shutdown_token)
        .stdin(Stdio::null())
        .stdout(Stdio::from(log.try_clone()?))
        .stderr(Stdio::from(log.try_clone()?));
    if let Some(web_root) = &cli.web_root {
        command.arg("--web-root").arg(web_root);
    }
    if let Some(serial_log_dir) = &cli.serial_log_dir {
        command.arg("--serial-log-dir").arg(serial_log_dir);
    }
    command.spawn().with_context(|| {
        format!(
            "start Web, Serial Broker and MCP through {}",
            binary.display()
        )
    })
}

fn stop_managed_host(child: &mut Child, address: SocketAddr, shutdown_token: &str, log: &mut File) {
    if child.try_wait().ok().flatten().is_some() {
        return;
    }
    if request_managed_shutdown(address, shutdown_token) {
        let deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < deadline {
            if child.try_wait().ok().flatten().is_some() {
                return;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        let _ = writeln!(
            log,
            "managed Host did not exit after graceful shutdown timeout"
        );
    } else {
        let _ = writeln!(
            log,
            "managed Host rejected or missed graceful shutdown request"
        );
    }
    let _ = child.kill();
    let _ = child.wait();
}

fn request_managed_shutdown(address: SocketAddr, shutdown_token: &str) -> bool {
    let Ok(mut stream) = TcpStream::connect_timeout(&address, HEALTH_TIMEOUT) else {
        return false;
    };
    let _ = stream.set_read_timeout(Some(HEALTH_TIMEOUT));
    let _ = stream.set_write_timeout(Some(HEALTH_TIMEOUT));
    let request = format!(
        "POST /host/api/v1/shutdown HTTP/1.1\r\nHost: {address}\r\nX-Linkr-Shutdown-Token: {shutdown_token}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
    );
    if stream.write_all(request.as_bytes()).is_err() {
        return false;
    }
    let mut response = String::new();
    stream.read_to_string(&mut response).is_ok()
        && response
            .lines()
            .next()
            .is_some_and(|line| line.starts_with("HTTP/1.1 202"))
}

fn host_is_healthy(address: SocketAddr) -> bool {
    let Ok(mut stream) = TcpStream::connect_timeout(&address, HEALTH_TIMEOUT) else {
        return false;
    };
    let _ = stream.set_read_timeout(Some(HEALTH_TIMEOUT));
    let _ = stream.set_write_timeout(Some(HEALTH_TIMEOUT));
    if stream
        .write_all(
            format!("GET /healthz HTTP/1.1\r\nHost: {address}\r\nConnection: close\r\n\r\n")
                .as_bytes(),
        )
        .is_err()
    {
        return false;
    }
    let mut response = String::new();
    if stream.read_to_string(&mut response).is_err() {
        return false;
    }
    let Some((headers, body)) = response.split_once("\r\n\r\n") else {
        return false;
    };
    if !headers.starts_with("HTTP/1.1 200") {
        return false;
    }
    host_health_is_acceptable(body)
}

fn host_health_is_acceptable(body: &str) -> bool {
    serde_json::from_str::<Value>(body).is_ok_and(|value| {
        value.get("ok").and_then(Value::as_bool) == Some(true)
            && value.get("schema").and_then(Value::as_str) == Some(GATEWAY_SCHEMA)
            && value.get("mcp_endpoint").and_then(Value::as_str) == Some("/mcp")
            && value
                .pointer("/serial_logging/enabled")
                .and_then(Value::as_bool)
                == Some(true)
    })
}

fn update_tray(tray: &TrayIcon, items: &MenuItems, state: ServiceState, managed: bool) {
    let (status, tooltip) = match state {
        ServiceState::Starting => (
            "Starting Web / Broker / MCP…",
            "Radxa Linkr: starting Web / Broker / MCP",
        ),
        ServiceState::Ready => (
            "Web / Broker / MCP are running",
            "Radxa Linkr: Web / Broker / MCP running",
        ),
        ServiceState::Offline => (
            "Web / Broker / MCP are unavailable",
            "Radxa Linkr: services unavailable",
        ),
    };
    items.status.set_text(status);
    items.restart.set_enabled(managed);
    let _ = tray.set_tooltip(Some(tooltip));
    if let Ok(icon) = status_icon(state) {
        let _ = tray.set_icon(Some(icon));
    }
}

fn status_icon(state: ServiceState) -> Result<Icon> {
    let color = match state {
        ServiceState::Starting => [245, 158, 11, 255],
        ServiceState::Ready => [34, 197, 94, 255],
        ServiceState::Offline => [239, 68, 68, 255],
    };
    let size = 32_u32;
    let mut rgba = vec![0_u8; (size * size * 4) as usize];
    for y in 0..size {
        for x in 0..size {
            let dx = x as i32 - 15;
            let dy = y as i32 - 15;
            let inside = dx * dx + dy * dy <= 13 * 13;
            let mark = ((10..=13).contains(&x) && (8..=23).contains(&y))
                || ((10..=22).contains(&x) && (20..=23).contains(&y));
            let pixel = ((y * size + x) * 4) as usize;
            let value = if mark && inside {
                [255, 255, 255, 255]
            } else if inside {
                color
            } else {
                [0, 0, 0, 0]
            };
            rgba[pixel..pixel + 4].copy_from_slice(&value);
        }
    }
    Icon::from_rgba(rgba, size, size).context("build tray status icon")
}

fn sibling_binary(name: &str) -> Result<PathBuf> {
    let executable = std::env::current_exe().context("resolve tray executable")?;
    let suffix = std::env::consts::EXE_SUFFIX;
    Ok(executable.with_file_name(format!("{name}{suffix}")))
}

fn app_data_dir() -> Result<PathBuf> {
    dirs::data_local_dir()
        .map(|path| path.join("Radxa Linkr Debugger"))
        .context("cannot determine per-user application data directory")
}

fn open_log(data_dir: &std::path::Path) -> Result<File> {
    let log_path = data_dir.join("host.log");
    if std::fs::metadata(&log_path).is_ok_and(|metadata| metadata.len() > 5 * 1024 * 1024) {
        let _ = std::fs::rename(&log_path, data_dir.join("host.log.old"));
    }
    OpenOptions::new()
        .create(true)
        .append(true)
        .open(log_path)
        .context("open Host log")
}

fn open_browser(url: &str) -> Result<()> {
    #[cfg(target_os = "macos")]
    let mut command = Command::new("open");
    #[cfg(target_os = "windows")]
    let mut command = {
        let mut command = Command::new("cmd");
        command.arg("/C").arg("start").arg("");
        command
    };
    #[cfg(all(unix, not(target_os = "macos")))]
    let mut command = Command::new("xdg-open");

    command.arg(url);
    command
        .spawn()
        .with_context(|| format!("open {url} in the default browser"))?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::host_health_is_acceptable;

    #[test]
    fn tray_only_adopts_hosts_with_uart_archiving_enabled() {
        let enabled = r#"{
            "schema":"radxa-linkr-debugger-gateway.v1",
            "ok":true,
            "mcp_endpoint":"/mcp",
            "serial_logging":{"enabled":true}
        }"#;
        let disabled = enabled.replace("\"enabled\":true", "\"enabled\":false");
        let missing = enabled.replace(",\n            \"serial_logging\":{\"enabled\":true}", "");

        assert!(host_health_is_acceptable(enabled));
        assert!(!host_health_is_acceptable(&disabled));
        assert!(!host_health_is_acceptable(&missing));
    }
}
