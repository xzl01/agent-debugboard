use std::{net::SocketAddr, path::PathBuf, process::Command, time::Duration};

use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand};
use radxa_linkr_host::{
    config::{HostConfig, ServeOptions, DEFAULT_PORT},
    gateway, mcp,
};
use reqwest::StatusCode;
use serde_json::{json, Value};
use tracing_subscriber::EnvFilter;
use url::Url;

#[derive(Debug, Parser)]
#[command(
    name = "linkr-host",
    version,
    about = "Radxa Linkr Debugger local host tools"
)]
struct Cli {
    #[command(subcommand)]
    command: Option<Commands>,
}

#[derive(Debug, Subcommand)]
enum Commands {
    /// Start the Web UI, board gateway and shared Serial Broker.
    Serve(ServeOptions),
    /// Open the locally hosted Web UI in the default browser.
    Open {
        #[arg(long, default_value_t = DEFAULT_PORT)]
        port: u16,
    },
    /// Show status for the running host service.
    Status {
        #[arg(long, default_value_t = DEFAULT_PORT)]
        port: u16,
        #[arg(long)]
        json: bool,
    },
    /// Diagnose the host service, Web assets, board API and CH347F ports.
    Doctor {
        #[arg(long, default_value_t = DEFAULT_PORT)]
        port: u16,
        #[arg(long)]
        web_root: Option<PathBuf>,
        #[arg(long)]
        json: bool,
    },
    /// Run the generic MCP stdio adapter for Codex, OpenCode and other clients.
    Mcp {
        #[command(flatten)]
        host: ServeOptions,

        /// Board API base exposed by Linkr Host. Defaults to the selected local port.
        #[arg(long, env = "LINKR_MCP_API_BASE")]
        api_base: Option<Url>,

        /// Shared Serial Broker WebSocket. Defaults to the selected local port.
        #[arg(long, env = "LINKR_MCP_SERIAL_URL")]
        serial_url: Option<Url>,

        /// Do not manage Linkr Host; MCP remains available while an external Host is offline.
        #[arg(long)]
        no_autostart: bool,
    },
}

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| EnvFilter::new("radxa_linkr_host=info")),
        )
        .with_writer(std::io::stderr)
        .init();
    let cli = Cli::parse();
    match cli
        .command
        .unwrap_or_else(|| Commands::Serve(ServeOptions::default()))
    {
        Commands::Serve(options) => gateway::serve(HostConfig::from_options(options)?).await,
        Commands::Open { port } => open_browser(&format!("http://127.0.0.1:{port}/")),
        Commands::Status { port, json } => show_status(port, json).await,
        Commands::Doctor {
            port,
            web_root,
            json,
        } => doctor(port, web_root, json).await,
        Commands::Mcp {
            host,
            api_base,
            serial_url,
            no_autostart,
        } => run_mcp(host, api_base, serial_url, no_autostart).await,
    }
}

async fn run_mcp(
    host: ServeOptions,
    api_base: Option<Url>,
    serial_url: Option<Url>,
    no_autostart: bool,
) -> Result<()> {
    let local_address = SocketAddr::new(host.host, host.port);
    let local_base = format!("http://{local_address}");
    let api_base = api_base.unwrap_or(Url::parse(&format!("{local_base}/api/v1/"))?);
    let serial_url = serial_url.unwrap_or(Url::parse(&format!("ws://{local_address}/serial"))?);

    // The MCP stdio handshake must not depend on Web assets, a free gateway
    // port, USB-NCM, or the debugger being present. MCP clients commonly
    // disable a server for the rest of their session when the child exits
    // during initialization, so Host startup is supervised independently.
    let host_supervisor =
        (!no_autostart).then(|| tokio::spawn(supervise_host(host, local_base.clone())));
    if no_autostart {
        tracing::info!(
            host = %local_base,
            "MCP started without Host autostart; tools will recover when the external Host appears"
        );
    }

    let result = mcp::serve_stdio(api_base, serial_url).await;
    if let Some(task) = host_supervisor {
        task.abort();
        let _ = task.await;
    }
    result
}

const HOST_HEALTH_POLL_INTERVAL: Duration = Duration::from_secs(2);
const HOST_READY_TIMEOUT: Duration = Duration::from_secs(5);
const HOST_RETRY_INITIAL: Duration = Duration::from_millis(250);
const HOST_RETRY_MAX: Duration = Duration::from_secs(30);

async fn supervise_host(options: ServeOptions, local_base: String) {
    let mut retry_delay = HOST_RETRY_INITIAL;
    loop {
        if host_is_healthy(options.host, options.port).await {
            retry_delay = HOST_RETRY_INITIAL;
            tokio::time::sleep(HOST_HEALTH_POLL_INTERVAL).await;
            continue;
        }

        let result = run_managed_host(options.clone(), &local_base, &mut retry_delay).await;
        match result {
            Ok(()) => tracing::warn!(host = %local_base, "managed Linkr Host stopped"),
            Err(error) => tracing::warn!(
                host = %local_base,
                retry_ms = retry_delay.as_millis(),
                error = %error,
                "managed Linkr Host unavailable; retrying in background"
            ),
        }
        tokio::time::sleep(retry_delay).await;
        retry_delay = retry_delay.saturating_mul(2).min(HOST_RETRY_MAX);
    }
}

async fn run_managed_host(
    options: ServeOptions,
    local_base: &str,
    retry_delay: &mut Duration,
) -> Result<()> {
    let host = options.host;
    let port = options.port;
    let config = HostConfig::from_options(options)?;
    let gateway = ManagedHostTask::spawn(config);
    let ready_deadline = tokio::time::Instant::now() + HOST_READY_TIMEOUT;

    loop {
        if gateway.is_finished() {
            return gateway.join().await;
        }
        if host_is_healthy(host, port).await {
            tracing::info!(host = %local_base, "managed Linkr Host is ready");
            *retry_delay = HOST_RETRY_INITIAL;
            break;
        }
        if tokio::time::Instant::now() >= ready_deadline {
            bail!("Linkr Host did not become ready within 5 seconds");
        }
        tokio::time::sleep(Duration::from_millis(100)).await;
    }

    gateway.join().await
}

struct ManagedHostTask(Option<tokio::task::JoinHandle<Result<()>>>);

impl ManagedHostTask {
    fn spawn(config: HostConfig) -> Self {
        Self(Some(tokio::spawn(
            async move { gateway::serve(config).await },
        )))
    }

    fn is_finished(&self) -> bool {
        self.0
            .as_ref()
            .is_none_or(tokio::task::JoinHandle::is_finished)
    }

    async fn join(mut self) -> Result<()> {
        self.0
            .take()
            .expect("managed Host task exists")
            .await
            .context("join managed Linkr Host task")?
            .context("managed Linkr Host exited")
    }
}

impl Drop for ManagedHostTask {
    fn drop(&mut self) {
        if let Some(task) = self.0.take() {
            task.abort();
        }
    }
}

async fn host_is_healthy(host: std::net::IpAddr, port: u16) -> bool {
    let address = SocketAddr::new(host, port);
    fetch_health_json(&format!("http://{address}/healthz"))
        .await
        .is_ok_and(|(status, value)| {
            status.is_success()
                && value.get("schema").and_then(Value::as_str)
                    == Some("radxa-linkr-debugger-gateway.v1")
        })
}

async fn fetch_health_json(url: &str) -> Result<(StatusCode, Value)> {
    let response = reqwest::Client::builder()
        .connect_timeout(Duration::from_millis(250))
        .timeout(Duration::from_millis(750))
        .build()?
        .get(url)
        .send()
        .await
        .with_context(|| format!("request {url}"))?;
    let status = response.status();
    let value = response
        .json()
        .await
        .with_context(|| format!("decode JSON from {url}"))?;
    Ok((status, value))
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
    let status = command.arg(url).status().context("open default browser")?;
    if !status.success() {
        bail!("browser launcher exited with {status}");
    }
    Ok(())
}

async fn fetch_json(url: &str) -> Result<(StatusCode, Value)> {
    let response = reqwest::Client::builder()
        .connect_timeout(std::time::Duration::from_secs(2))
        .timeout(std::time::Duration::from_secs(8))
        .build()?
        .get(url)
        .send()
        .await
        .with_context(|| format!("request {url}"))?;
    let status = response.status();
    let value = response
        .json()
        .await
        .with_context(|| format!("decode JSON from {url}"))?;
    Ok((status, value))
}

async fn show_status(port: u16, json_output: bool) -> Result<()> {
    let url = format!("http://127.0.0.1:{port}/host/api/v1/status");
    let (status, value) = fetch_json(&url).await?;
    if !status.is_success() {
        bail!("Linkr Host returned HTTP {status}: {value}");
    }
    if json_output {
        println!("{}", serde_json::to_string_pretty(&value)?);
    } else {
        println!(
            "Linkr Host {}",
            value
                .get("version")
                .and_then(Value::as_str)
                .unwrap_or("unknown")
        );
        println!(
            "  URL:   {}",
            value.get("listen").and_then(Value::as_str).unwrap_or("-")
        );
        println!(
            "  Board: {}",
            value
                .get("board_url")
                .and_then(Value::as_str)
                .unwrap_or("-")
        );
        println!(
            "  Web:   {}",
            value.get("web_root").and_then(Value::as_str).unwrap_or("-")
        );
    }
    Ok(())
}

async fn doctor(port: u16, web_root: Option<PathBuf>, json_output: bool) -> Result<()> {
    let host_url = format!("http://127.0.0.1:{port}");
    let host = fetch_json(&format!("{host_url}/healthz")).await;
    let board = fetch_json(&format!("{host_url}/api/v1/status")).await;
    let discovered_web = web_root
        .map(Ok)
        .unwrap_or_else(radxa_linkr_host::config::discover_web_root);
    let ports = tokio::task::spawn_blocking(tokio_serial::available_ports)
        .await
        .context("enumerate serial ports")?;
    let serial = ports
        .map(|ports| ports.into_iter().filter(|port| matches!(&port.port_type, tokio_serial::SerialPortType::UsbPort(info) if info.vid == 0x1a86)).map(|port| port.port_name).collect::<Vec<_>>())
        .unwrap_or_default();
    let host_ok = host.as_ref().is_ok_and(|(status, _)| status.is_success());
    let board_ok = board.as_ref().is_ok_and(|(status, _)| status.is_success());
    let web_ok = discovered_web
        .as_ref()
        .is_ok_and(|path| path.join("index.html").is_file());
    let serial_ok = serial.len() >= 2;
    let result = json!({
        "schema": "radxa-linkr-host-doctor.v1",
        "ok": all_doctor_checks_ok([host_ok, board_ok, web_ok, serial_ok]),
        "host": match host { Ok((status, body)) => json!({"ok": status.is_success(), "status": status.as_u16(), "body": body}), Err(error) => json!({"ok": false, "error": error.to_string()}) },
        "board": match board { Ok((status, body)) => json!({"ok": status.is_success(), "status": status.as_u16(), "body": body}), Err(error) => json!({"ok": false, "error": error.to_string()}) },
        "web": match discovered_web { Ok(path) => json!({"ok": path.join("index.html").is_file(), "path": path}), Err(error) => json!({"ok": false, "error": error.to_string()}) },
        "serial": {"ok": serial_ok, "ch347_ports": serial}
    });
    if json_output {
        println!("{}", serde_json::to_string_pretty(&result)?);
    } else {
        println!("Linkr Host doctor");
        for key in ["host", "board", "web", "serial"] {
            let item = &result[key];
            println!(
                "  {:<7} {}",
                key,
                if item["ok"].as_bool().unwrap_or(false) {
                    "OK"
                } else {
                    "FAIL"
                }
            );
        }
    }
    if result["ok"].as_bool() == Some(true) {
        Ok(())
    } else {
        bail!("one or more Linkr Host checks failed")
    }
}

fn all_doctor_checks_ok(checks: [bool; 4]) -> bool {
    checks.into_iter().all(|check| check)
}

#[cfg(test)]
mod tests {
    use super::all_doctor_checks_ok;

    #[test]
    fn doctor_requires_every_host_surface() {
        assert!(all_doctor_checks_ok([true, true, true, true]));
        for index in 0..4 {
            let mut checks = [true; 4];
            checks[index] = false;
            assert!(!all_doctor_checks_ok(checks));
        }
    }
}
