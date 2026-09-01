use std::{
    fs::{File, OpenOptions},
    io::{Read, Seek, Write},
    net::{IpAddr, Ipv4Addr, SocketAddr, TcpStream},
    path::PathBuf,
    process::Command,
    time::{Duration, Instant},
};

use anyhow::{Context, Result};
use clap::Parser;
use fs2::FileExt;
use serde_json::Value;
use tao::{
    event::Event,
    event_loop::{ControlFlow, EventLoopBuilder},
};
use tokio::task::JoinHandle;
use tray_icon::{
    menu::{Menu, MenuEvent, MenuItem, PredefinedMenuItem},
    TrayIcon, TrayIconBuilder,
};
use uuid::Uuid;

use radxa_linkr_host::{
    config::{HostConfig, SerialLogMode, ServeOptions},
    gateway,
};

#[path = "linkr_tray/firmware_activity.rs"]
mod firmware_activity;
#[path = "linkr_tray/icon.rs"]
mod icon;
#[path = "linkr_tray/icon_publish.rs"]
mod icon_publish;

use firmware_activity::{
    complete_host_activity_probe, complete_probe, run_indicator_probes, HostIndicatorSnapshot,
    IndicatorSnapshot,
};
use icon::{changed_status_icon, status_icon, status_rgba, IconFrame, IndicatorState};
use icon_publish::IconPublisher;

const HEALTH_INTERVAL: Duration = Duration::from_secs(2);
const HEALTH_TIMEOUT: Duration = Duration::from_millis(500);
const BOARD_STATUS_INTERVAL: Duration = Duration::from_millis(100);
const HOST_ACTIVITY_INTERVAL: Duration = Duration::from_millis(100);
const CONNECTION_ANIMATION_INTERVAL: Duration = Duration::from_millis(300);
const OFFLINE_ANIMATION_INTERVAL: Duration = Duration::from_millis(600);
const READY_ANIMATION_INTERVAL: Duration = Duration::from_millis(100);
const LOGIC_ANALYZER_ANIMATION_INTERVAL: Duration = Duration::from_millis(60);
// ponytail: fixed wake keeps Linux tray animation independent of desktop input;
// replace with a deadline channel only if the 50 wakeups/s cost becomes measurable.
const ANIMATION_WAKE_INTERVAL: Duration = Duration::from_millis(20);
const RESTART_INITIAL: Duration = Duration::from_millis(500);
const RESTART_MAX: Duration = Duration::from_secs(30);
const SHUTDOWN_REQUEST_INTERVAL: Duration = Duration::from_millis(100);
const TRAY_PROMOTION_TIMEOUT: Duration = Duration::from_secs(6);
const TRAY_PROMOTION_POLL: Duration = Duration::from_millis(50);
const GATEWAY_SCHEMA: &str = "radxa-linkr-debugger-gateway.v1";

#[derive(Debug, Parser)]
#[command(
    name = "linkr-tray",
    version,
    about = "Tray supervisor for Radxa Linkr Debugger Web, Broker and MCP services"
)]
struct Cli {
    #[command(flatten)]
    host: ServeOptions,

    /// Run the Host daemon without creating a desktop tray icon.
    #[arg(long)]
    headless: bool,

    /// Exit silently when another tray instance owns the per-user lock.
    #[arg(long)]
    no_open_when_running: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ServiceState {
    Starting,
    Ready,
    Offline,
}

fn icon_animation_interval(state: ServiceState, indicator: IndicatorState) -> Duration {
    match state {
        ServiceState::Ready if indicator.logic_analyzer_active() => {
            LOGIC_ANALYZER_ANIMATION_INTERVAL
        }
        ServiceState::Ready => READY_ANIMATION_INTERVAL,
        ServiceState::Starting => CONNECTION_ANIMATION_INTERVAL,
        ServiceState::Offline => OFFLINE_ANIMATION_INTERVAL,
    }
}

const fn icon_state(
    service_state: ServiceState,
    board_status_known: bool,
    indicator: IndicatorState,
) -> ServiceState {
    match service_state {
        ServiceState::Ready if !board_status_known => ServiceState::Starting,
        ServiceState::Ready if indicator.board_online() => ServiceState::Ready,
        ServiceState::Ready => ServiceState::Offline,
        ServiceState::Starting => ServiceState::Starting,
        ServiceState::Offline => ServiceState::Offline,
    }
}

const fn animation_mode(
    state: ServiceState,
    indicator: IndicatorState,
) -> (ServiceState, (bool, bool)) {
    (state, indicator.host_activity())
}

fn schedule_control_flow(now: Instant, deadline: Instant) -> ControlFlow {
    if deadline <= now {
        ControlFlow::Poll
    } else {
        ControlFlow::WaitUntil(deadline)
    }
}

fn run_animation_waker(mut wake: impl FnMut() -> bool) {
    loop {
        std::thread::sleep(ANIMATION_WAKE_INTERVAL);
        if !wake() {
            return;
        }
    }
}

#[derive(Debug)]
enum UserEvent {
    Menu(MenuEvent),
    IndicatorProbe {
        firmware: Option<IndicatorSnapshot>,
        host: Option<HostIndicatorSnapshot>,
    },
    AnimationWake,
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

// allow: SIZE_OK — one Tao desktop state machine; service and probe work live in modules.
#[tokio::main(flavor = "multi_thread", worker_threads = 4)]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("radxa_linkr_host=info")),
        )
        .with_writer(std::io::stderr)
        .init();
    let mut cli = Cli::parse();
    if cli.host.serial_log_mode.is_none() {
        cli.host.serial_log_mode = Some(SerialLogMode::Rx);
    }
    let port = cli.host.port;
    let board_url = cli.host.board_url.clone();
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
    let show_tray_request = data_dir.join("show-tray.request");
    let mut owns_lock = lock.try_lock_exclusive().is_ok();
    if !owns_lock && !cli.headless && lock_owner_is_headless(&mut lock) {
        std::fs::write(&show_tray_request, b"show\n")
            .context("request headless daemon handoff to graphical tray")?;
        let deadline = Instant::now() + TRAY_PROMOTION_TIMEOUT;
        while Instant::now() < deadline {
            if lock.try_lock_exclusive().is_ok() {
                owns_lock = true;
                break;
            }
            std::thread::sleep(TRAY_PROMOTION_POLL);
        }
        if !owns_lock {
            let _ = std::fs::remove_file(&show_tray_request);
        }
    }
    if !owns_lock {
        if !cli.no_open_when_running {
            open_browser(&format!("http://127.0.0.1:{port}/"))?;
        }
        return Ok(());
    }
    let mode = if cli.headless { "headless" } else { "starting" };
    write_lock_owner(&mut lock, mode)?;
    let shutdown_request = data_dir.join("shutdown.request");
    let _ = std::fs::remove_file(&shutdown_request);
    let _ = std::fs::remove_file(&show_tray_request);
    let health_addr = SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), port);
    let existing_host_healthy = host_is_healthy(health_addr);
    if cli.headless && existing_host_healthy {
        return Ok(());
    }
    let shutdown_token = Uuid::new_v4().to_string();
    cli.host.shutdown_token = Some(shutdown_token.clone());
    let host_config = HostConfig::from_options(cli.host.clone())?;
    if cli.headless {
        let shutdown_request = shutdown_request.clone();
        let show_tray_request = show_tray_request.clone();
        return gateway::serve_with_shutdown(host_config, async move {
            tokio::select! {
                _ = tokio::signal::ctrl_c() => {}
                _ = wait_for_daemon_request(shutdown_request, show_tray_request) => {}
            }
        })
        .await;
    }
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
    let initial_frame = IconFrame {
        state: ServiceState::Starting,
        animation: 0,
        indicator: IndicatorState::default(),
    };
    let mut icon_publisher = IconPublisher::new(&data_dir)?;
    let tray = icon_publisher
        .configure(TrayIconBuilder::new())
        .with_menu(Box::new(menu))
        .with_tooltip("Radxa Linkr: starting Web / Broker / MCP")
        .with_icon(status_icon(initial_frame)?)
        .build()
        .context("create tray icon")?;
    icon_publisher.protect_initial()?;
    write_lock_owner(&mut lock, "tray")?;

    let indicator_proxy = event_loop.create_proxy();
    // Process-lifetime task exits when the Tao event proxy closes.
    let _indicator_task = tokio::spawn(run_indicator_probes(
        indicator_proxy,
        board_url,
        health_addr,
    ));
    let proxy = event_loop.create_proxy();
    MenuEvent::set_event_handler(Some(move |event| {
        let _ = proxy.send_event(UserEvent::Menu(event));
    }));
    let shutdown_proxy = event_loop.create_proxy();
    ctrlc::set_handler(move || {
        let _ = shutdown_proxy.send_event(UserEvent::Shutdown);
    })
    .context("install tray shutdown handler")?;
    let animation_proxy = event_loop.create_proxy();
    std::thread::Builder::new()
        .name("linkr-tray-animation".to_string())
        .spawn(move || {
            run_animation_waker(|| animation_proxy.send_event(UserEvent::AnimationWake).is_ok());
        })
        .context("start tray animation wake timer")?;

    let web_url = format!("http://127.0.0.1:{port}/");
    let status_url = format!("http://127.0.0.1:{port}/host/api/v1/status");
    let serial_logs_url = format!("http://127.0.0.1:{port}/#serial-logs");
    let mut managed = (!existing_host_healthy).then(|| spawn_host(host_config.clone()));
    let mut last_state = ServiceState::Starting;
    let mut next_health = Instant::now();
    let mut next_animation = Instant::now();
    let mut animation_frame = 0_u8;
    let mut indicator = IndicatorState::default();
    let mut board_status_known = false;
    let mut published_rgba = status_rgba(initial_frame);
    let mut next_restart = Instant::now();
    let mut restart_delay = RESTART_INITIAL;
    let mut shutting_down = false;

    event_loop.run(move |event, _, control_flow| {
        *control_flow = schedule_control_flow(Instant::now(), next_health.min(next_animation));
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
                if let Some(mut host) = managed.take() {
                    stop_managed_host(&mut host, health_addr, &shutdown_token);
                }
                next_restart = Instant::now();
                restart_delay = RESTART_INITIAL;
                update_tray(&tray, &items, ServiceState::Starting, false);
                last_state = ServiceState::Starting;
                indicator = IndicatorState::default();
                board_status_known = false;
                animation_frame = 0;
                next_animation = Instant::now();
            }
            Event::UserEvent(UserEvent::IndicatorProbe { firmware, host }) => {
                if last_state == ServiceState::Ready {
                    let previous_mode = animation_mode(
                        icon_state(last_state, board_status_known, indicator),
                        indicator,
                    );
                    board_status_known = true;
                    indicator = complete_host_activity_probe(complete_probe(firmware), host);
                    let state = icon_state(last_state, board_status_known, indicator);
                    let restart_animation = previous_mode != animation_mode(state, indicator);
                    if restart_animation {
                        animation_frame = 0;
                    }
                    publish_status_icon(
                        &tray,
                        &mut icon_publisher,
                        IconFrame {
                            state,
                            animation: animation_frame,
                            indicator,
                        },
                        &mut published_rgba,
                    );
                    if restart_animation {
                        next_animation = Instant::now() + icon_animation_interval(state, indicator);
                    }
                } else {
                    indicator = IndicatorState::default();
                    board_status_known = false;
                }
                *control_flow =
                    schedule_control_flow(Instant::now(), next_health.min(next_animation));
            }
            Event::UserEvent(UserEvent::AnimationWake) => {}
            Event::UserEvent(UserEvent::Menu(event)) if event.id == *items.quit.id() => {
                shutting_down = true;
                if let Some(mut host) = managed.take() {
                    stop_managed_host(&mut host, health_addr, &shutdown_token);
                }
                *control_flow = ControlFlow::Exit;
            }
            Event::UserEvent(UserEvent::Shutdown) => {
                shutting_down = true;
                if let Some(mut host) = managed.take() {
                    stop_managed_host(&mut host, health_addr, &shutdown_token);
                }
                *control_flow = ControlFlow::Exit;
            }
            Event::MainEventsCleared if shutdown_request.is_file() => {
                shutting_down = true;
                let _ = std::fs::remove_file(&shutdown_request);
                if let Some(mut host) = managed.take() {
                    stop_managed_host(&mut host, health_addr, &shutdown_token);
                }
                *control_flow = ControlFlow::Exit;
            }
            Event::MainEventsCleared
                if !shutting_down
                    && (Instant::now() >= next_health || Instant::now() >= next_animation) =>
            {
                let now = Instant::now();
                if now >= next_health {
                    if managed.as_ref().is_some_and(JoinHandle::is_finished) {
                        managed = None;
                    }

                    let healthy = host_is_healthy(health_addr);
                    let state = if healthy {
                        restart_delay = RESTART_INITIAL;
                        ServiceState::Ready
                    } else {
                        if managed.is_none() && now >= next_restart {
                            managed = Some(spawn_host(host_config.clone()));
                            next_restart = now + restart_delay;
                            restart_delay = restart_delay.saturating_mul(2).min(RESTART_MAX);
                        }
                        if managed.is_some() {
                            ServiceState::Starting
                        } else {
                            ServiceState::Offline
                        }
                    };
                    if state != last_state {
                        update_tray(&tray, &items, state, managed.is_some());
                        last_state = state;
                        indicator = IndicatorState::default();
                        board_status_known = false;
                        animation_frame = 0;
                        next_animation = now;
                    } else {
                        items.restart.set_enabled(managed.is_some());
                    }
                    next_health = now + HEALTH_INTERVAL;
                }

                if now >= next_animation {
                    let state = icon_state(last_state, board_status_known, indicator);
                    publish_status_icon(
                        &tray,
                        &mut icon_publisher,
                        IconFrame {
                            state,
                            animation: animation_frame,
                            indicator,
                        },
                        &mut published_rgba,
                    );
                    animation_frame = animation_frame.wrapping_add(1);
                    next_animation = now + icon_animation_interval(state, indicator);
                }
                *control_flow =
                    schedule_control_flow(Instant::now(), next_health.min(next_animation));
            }
            _ => {}
        }
    });
}

fn spawn_host(config: HostConfig) -> JoinHandle<()> {
    tokio::spawn(async move {
        if let Err(error) = gateway::serve_managed(config).await {
            tracing::error!(error = %error, "managed Linkr Host stopped");
        }
    })
}

fn stop_managed_host(task: &mut JoinHandle<()>, address: SocketAddr, shutdown_token: &str) {
    if task.is_finished() {
        return;
    }
    if request_managed_shutdown(address, shutdown_token) {
        let deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < deadline {
            if task.is_finished() {
                return;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        tracing::warn!("managed Linkr Host missed graceful shutdown deadline");
    } else {
        tracing::warn!("managed Linkr Host rejected or missed graceful shutdown request");
    }
    task.abort();
    let deadline = Instant::now() + Duration::from_secs(1);
    while !task.is_finished() && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(10));
    }
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

fn lock_owner_is_headless(lock: &mut File) -> bool {
    if lock.rewind().is_err() {
        return false;
    }
    let mut owner = String::new();
    lock.read_to_string(&mut owner).is_ok() && owner.split_whitespace().nth(1) == Some("headless")
}

fn write_lock_owner(lock: &mut File, mode: &str) -> Result<()> {
    lock.rewind().context("rewind tray instance lock")?;
    lock.set_len(0).context("reset tray instance lock")?;
    writeln!(lock, "{} {mode}", std::process::id()).context("write tray process id and mode")?;
    lock.sync_data().context("persist tray process id")
}

async fn wait_for_daemon_request(shutdown: PathBuf, show_tray: PathBuf) {
    loop {
        if shutdown.is_file() {
            let _ = std::fs::remove_file(shutdown);
            return;
        }
        if show_tray.is_file() {
            let _ = std::fs::remove_file(show_tray);
            tracing::info!("headless desktop daemon handing off to graphical tray");
            return;
        }
        tokio::time::sleep(SHUTDOWN_REQUEST_INTERVAL).await;
    }
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
}

fn publish_status_icon(
    tray: &TrayIcon,
    publisher: &mut IconPublisher,
    frame: IconFrame,
    published_rgba: &mut Vec<u8>,
) {
    if let Ok(Some(icon)) = changed_status_icon(frame, published_rgba) {
        let _ = publisher.set_icon(tray, icon);
    }
}

fn app_data_dir() -> Result<PathBuf> {
    dirs::data_local_dir()
        .map(|path| path.join("radxa-linkr-debugger"))
        .context("cannot determine per-user application data directory")
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
    use super::{
        host_health_is_acceptable, icon_animation_interval, icon_state, run_animation_waker,
        IndicatorState, ServiceState, ANIMATION_WAKE_INTERVAL, BOARD_STATUS_INTERVAL,
        CONNECTION_ANIMATION_INTERVAL, HEALTH_INTERVAL, HEALTH_TIMEOUT, HOST_ACTIVITY_INTERVAL,
        LOGIC_ANALYZER_ANIMATION_INTERVAL, OFFLINE_ANIMATION_INTERVAL, READY_ANIMATION_INTERVAL,
    };
    use std::ffi::OsStr;
    use std::time::Duration;
    use tao::event_loop::ControlFlow;

    #[test]
    fn tray_adopts_compatible_hosts_regardless_of_uart_archiving() {
        let enabled = r#"{
            "schema":"radxa-linkr-debugger-gateway.v1",
            "ok":true,
            "mcp_endpoint":"/mcp",
            "serial_logging":{"enabled":true}
        }"#;
        let disabled = enabled.replace("\"enabled\":true", "\"enabled\":false");
        let missing = enabled.replace(",\n            \"serial_logging\":{\"enabled\":true}", "");

        assert!(host_health_is_acceptable(enabled));
        assert!(host_health_is_acceptable(&disabled));
        assert!(host_health_is_acceptable(&missing));
    }

    #[test]
    fn desktop_data_paths_use_ascii_slug() {
        let tray = super::app_data_dir().unwrap();
        let serial = radxa_linkr_host::serial_log::default_log_root();

        assert_eq!(tray.file_name(), Some(OsStr::new("radxa-linkr-debugger")));
        assert_eq!(serial.parent(), Some(tray.as_path()));
    }

    #[test]
    fn logic_analyzer_uses_the_fastest_animation_interval() {
        let idle = IndicatorState::default();
        let logic = IndicatorState::new([false, false, false], false, true);

        assert!(
            icon_animation_interval(ServiceState::Ready, logic)
                < icon_animation_interval(ServiceState::Ready, idle)
        );
        assert!(
            icon_animation_interval(ServiceState::Ready, idle)
                < icon_animation_interval(ServiceState::Starting, idle)
        );
    }

    #[test]
    fn indicator_probe_is_responsive_without_speeding_up_health_checks() {
        assert_eq!(HOST_ACTIVITY_INTERVAL, Duration::from_millis(100));
        assert_eq!(BOARD_STATUS_INTERVAL, Duration::from_millis(100));
        assert!(HOST_ACTIVITY_INTERVAL < HEALTH_TIMEOUT);
        assert!(BOARD_STATUS_INTERVAL < HEALTH_TIMEOUT);
        assert!(HEALTH_TIMEOUT < HEALTH_INTERVAL);
    }

    #[test]
    fn tray_animation_periods_match_the_webui_contract() {
        assert_eq!(
            CONNECTION_ANIMATION_INTERVAL * 4,
            Duration::from_millis(1200)
        );
        assert_eq!(OFFLINE_ANIMATION_INTERVAL * 2, Duration::from_millis(1200));
        assert_eq!(READY_ANIMATION_INTERVAL * 8, Duration::from_millis(800));
        assert_eq!(
            LOGIC_ANALYZER_ANIMATION_INTERVAL * 8,
            Duration::from_millis(480)
        );
    }

    #[test]
    fn tray_connection_state_follows_the_webui_board_boundary() {
        let online = IndicatorState::new([false, false, false], false, false);

        assert_eq!(
            icon_state(ServiceState::Starting, false, IndicatorState::default()),
            ServiceState::Starting
        );
        assert_eq!(
            icon_state(ServiceState::Ready, false, IndicatorState::default()),
            ServiceState::Starting
        );
        assert_eq!(
            icon_state(ServiceState::Ready, true, IndicatorState::default()),
            ServiceState::Offline
        );
        assert_eq!(
            icon_state(ServiceState::Ready, true, online),
            ServiceState::Ready
        );
    }

    #[test]
    fn overdue_deadline_selects_poll_control_flow() {
        let now = std::time::Instant::now();
        let future = now + Duration::from_secs(1);

        assert!(matches!(
            super::schedule_control_flow(now, now),
            ControlFlow::Poll
        ));
        assert!(matches!(
            super::schedule_control_flow(now, future),
            ControlFlow::WaitUntil(deadline) if deadline == future
        ));
    }

    #[test]
    fn animation_waker_produces_ticks_without_external_events() {
        let mut wakes = 0;

        run_animation_waker(|| {
            wakes += 1;
            wakes < 2
        });

        assert_eq!(wakes, 2);
        assert_eq!(ANIMATION_WAKE_INTERVAL, Duration::from_millis(20));
        assert_eq!(
            LOGIC_ANALYZER_ANIMATION_INTERVAL.as_millis() % ANIMATION_WAKE_INTERVAL.as_millis(),
            0
        );
        assert_eq!(
            READY_ANIMATION_INTERVAL.as_millis() % ANIMATION_WAKE_INTERVAL.as_millis(),
            0
        );
    }

    #[test]
    fn rail_poll_updates_do_not_restart_the_webui_heartbeat_phase() {
        let first = IndicatorState::new([true, false, true], false, false);
        let rails_changed = IndicatorState::new([true, true, true], false, false);
        let uart_started = IndicatorState::new([true, true, true], true, false);

        assert_eq!(
            super::animation_mode(ServiceState::Ready, first),
            super::animation_mode(ServiceState::Ready, rails_changed)
        );
        assert_ne!(
            super::animation_mode(ServiceState::Ready, rails_changed),
            super::animation_mode(ServiceState::Ready, uart_started)
        );
    }
}
