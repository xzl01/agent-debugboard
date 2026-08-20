use std::{
    fs,
    net::TcpListener,
    sync::{
        atomic::{AtomicUsize, Ordering},
        Arc,
    },
    time::Duration,
};

use anyhow::{bail, Result};
use rmcp::{
    model::{
        CallToolRequestParams, NumberOrString, ProgressNotificationParam, ProgressToken,
        RequestParamsMeta,
    },
    transport::{ConfigureCommandExt, StreamableHttpClientTransport, TokioChildProcess},
    ClientHandler, ServiceExt,
};
use serde_json::json;

#[tokio::test]
async fn stdio_mcp_autostarts_host_and_lists_the_bounded_tools() -> Result<()> {
    let web_root = tempfile::tempdir()?;
    fs::write(web_root.path().join("index.html"), "<!doctype html>")?;

    let listener = TcpListener::bind("127.0.0.1:0")?;
    let port = listener.local_addr()?.port();
    drop(listener);

    let transport = TokioChildProcess::new(
        tokio::process::Command::new(env!("CARGO_BIN_EXE_linkr-host")).configure(|command| {
            command
                .arg("mcp")
                .arg("--port")
                .arg(port.to_string())
                .arg("--web-root")
                .arg(web_root.path())
                .arg("--board-url")
                .arg("http://127.0.0.1:1");
        }),
    )?;

    let client = ().serve(transport).await?;
    let peer_info = client
        .peer_info()
        .expect("MCP initialize retains peer metadata");
    let server_info = peer_info
        .server_info
        .as_ref()
        .expect("MCP initialize includes server metadata");
    assert_eq!(server_info.name, "radxa-linkr-debugger");
    assert_eq!(server_info.version, env!("CARGO_PKG_VERSION"));
    assert_tool_names(&client.list_all_tools().await?);
    client.cancel().await?;
    Ok(())
}

#[tokio::test]
async fn resident_host_exposes_streamable_http_mcp() -> Result<()> {
    let web_root = tempfile::tempdir()?;
    fs::write(web_root.path().join("index.html"), "<!doctype html>")?;
    let port = unused_port()?;

    let mut host = tokio::process::Command::new(env!("CARGO_BIN_EXE_linkr-host"))
        .arg("serve")
        .arg("--port")
        .arg(port.to_string())
        .arg("--web-root")
        .arg(web_root.path())
        .arg("--board-url")
        .arg("http://127.0.0.1:1")
        .kill_on_drop(true)
        .spawn()?;
    wait_for_host_health(port).await?;

    let transport = StreamableHttpClientTransport::from_uri(format!("http://127.0.0.1:{port}/mcp"));
    let client = tokio::time::timeout(Duration::from_secs(5), ().serve(transport)).await??;
    assert_tool_names(&client.list_all_tools().await?);
    client.cancel().await?;
    host.kill().await?;
    Ok(())
}

#[tokio::test]
async fn resident_host_exposes_local_only_serial_log_api() -> Result<()> {
    let web_root = tempfile::tempdir()?;
    let serial_logs = tempfile::tempdir()?;
    fs::write(web_root.path().join("index.html"), "<!doctype html>")?;
    let port = unused_port()?;

    let mut host = tokio::process::Command::new(env!("CARGO_BIN_EXE_linkr-host"))
        .arg("serve")
        .arg("--port")
        .arg(port.to_string())
        .arg("--web-root")
        .arg(web_root.path())
        .arg("--board-url")
        .arg("http://127.0.0.1:1")
        .arg("--serial-log-mode")
        .arg("rx")
        .arg("--serial-log-dir")
        .arg(serial_logs.path())
        .kill_on_drop(true)
        .spawn()?;
    wait_for_host_health(port).await?;

    let client = reqwest::Client::new();
    let base = format!("http://127.0.0.1:{port}/host/api/v1");
    let status = client
        .get(format!("{base}/serial-logging/status"))
        .send()
        .await?;
    assert!(status.status().is_success());
    let status = status.json::<serde_json::Value>().await?;
    assert_eq!(
        status.get("schema").and_then(|value| value.as_str()),
        Some("linkr-serial-log.v1")
    );
    assert_eq!(
        status.get("enabled").and_then(|value| value.as_bool()),
        Some(true)
    );

    let forbidden = client
        .get(format!("{base}/serial-logs"))
        .header("Origin", "https://xzl01.github.io")
        .send()
        .await?;
    assert_eq!(forbidden.status(), reqwest::StatusCode::FORBIDDEN);

    let navigation_forbidden = client
        .get(format!("{base}/serial-logs"))
        .header("Sec-Fetch-Site", "cross-site")
        .send()
        .await?;
    assert_eq!(
        navigation_forbidden.status(),
        reqwest::StatusCode::FORBIDDEN
    );

    let invalid = client
        .get(format!("{base}/serial-logs/not-a-uuid/download?format=raw"))
        .send()
        .await?;
    assert_eq!(invalid.status(), reqwest::StatusCode::BAD_REQUEST);

    host.kill().await?;
    Ok(())
}

#[tokio::test]
async fn managed_shutdown_requires_token_and_exits_cleanly() -> Result<()> {
    let web_root = tempfile::tempdir()?;
    fs::write(web_root.path().join("index.html"), "<!doctype html>")?;
    let port = unused_port()?;
    let shutdown_token = "test-managed-shutdown-token";

    let mut host = tokio::process::Command::new(env!("CARGO_BIN_EXE_linkr-host"))
        .arg("serve")
        .arg("--port")
        .arg(port.to_string())
        .arg("--web-root")
        .arg(web_root.path())
        .arg("--board-url")
        .arg("http://127.0.0.1:1")
        .env("LINKR_HOST_SHUTDOWN_TOKEN", shutdown_token)
        .kill_on_drop(true)
        .spawn()?;
    wait_for_host_health(port).await?;

    let client = reqwest::Client::new();
    let url = format!("http://127.0.0.1:{port}/host/api/v1/shutdown");
    let forbidden = client
        .post(&url)
        .header("X-Linkr-Shutdown-Token", "wrong-token")
        .send()
        .await?;
    assert_eq!(forbidden.status(), reqwest::StatusCode::FORBIDDEN);

    let accepted = client
        .post(&url)
        .header("X-Linkr-Shutdown-Token", shutdown_token)
        .send()
        .await?;
    assert_eq!(accepted.status(), reqwest::StatusCode::ACCEPTED);
    let status = tokio::time::timeout(Duration::from_secs(5), host.wait()).await??;
    assert!(status.success());
    Ok(())
}

#[tokio::test]
async fn stdio_mcp_initializes_without_an_external_host() -> Result<()> {
    let web_root = tempfile::tempdir()?;
    fs::write(web_root.path().join("index.html"), "<!doctype html>")?;
    let port = unused_port()?;

    let transport = TokioChildProcess::new(
        tokio::process::Command::new(env!("CARGO_BIN_EXE_linkr-host")).configure(|command| {
            command
                .arg("mcp")
                .arg("--no-autostart")
                .arg("--port")
                .arg(port.to_string())
                .arg("--web-root")
                .arg(web_root.path());
        }),
    )?;

    let progress = ProgressClient::default();
    let client =
        tokio::time::timeout(Duration::from_secs(5), progress.clone().serve(transport)).await??;
    assert_tool_names(&client.list_all_tools().await?);

    let result = client
        .call_tool(
            CallToolRequestParams::new("linkr_board_status").with_arguments(
                json!({"detail": "summary"})
                    .as_object()
                    .expect("tool arguments are an object")
                    .clone(),
            ),
        )
        .await?;
    assert_eq!(result.is_error, Some(true));
    let structured = result
        .structured_content
        .expect("temporary dependency failure is structured");
    assert_eq!(
        structured
            .pointer("/error/code")
            .and_then(|value| value.as_str()),
        Some("host_temporarily_unavailable")
    );
    assert_eq!(
        structured
            .pointer("/error/details/retryable")
            .and_then(|value| value.as_bool()),
        Some(true)
    );

    let write_result = client
        .call_tool(
            CallToolRequestParams::new("linkr_power_set").with_arguments(
                json!({"name": "5v_out", "state": "off", "confirm": true})
                    .as_object()
                    .expect("tool arguments are an object")
                    .clone(),
            ),
        )
        .await?;
    assert_eq!(write_result.is_error, Some(true));
    assert_eq!(
        write_result
            .structured_content
            .as_ref()
            .and_then(|value| value.pointer("/error/details/retryable"))
            .and_then(|value| value.as_bool()),
        Some(false),
        "hardware-changing tools must never advertise automatic replay"
    );

    let serial_write_result = client
        .call_tool(
            CallToolRequestParams::new("linkr_serial_write").with_arguments(
                json!({
                    "channel": "uart0",
                    "text": "reboot",
                    "line_ending": "cr",
                    "exclusive": true
                })
                .as_object()
                .expect("tool arguments are an object")
                .clone(),
            ),
        )
        .await?;
    assert_eq!(serial_write_result.is_error, Some(true));
    assert_eq!(
        serial_write_result
            .structured_content
            .as_ref()
            .and_then(|value| value.pointer("/error/details/retryable"))
            .and_then(|value| value.as_bool()),
        Some(false),
        "serial writes must never advertise automatic replay"
    );

    let mut serial_request = CallToolRequestParams::new("linkr_serial_expect").with_arguments(
        json!({"channel": "uart0", "pattern": "login:", "timeout_ms": 1_000})
            .as_object()
            .expect("tool arguments are an object")
            .clone(),
    );
    serial_request.set_progress_token(ProgressToken(NumberOrString::String(
        "serial-wait-test".into(),
    )));
    let serial_result = client.call_tool(serial_request).await?;
    assert_eq!(serial_result.is_error, Some(true));
    assert!(
        progress.notifications.load(Ordering::SeqCst) >= 2,
        "long-running serial tools should report start and completion progress"
    );

    client.cancel().await?;
    Ok(())
}

#[tokio::test]
async fn stdio_mcp_recovers_after_the_host_port_becomes_available() -> Result<()> {
    let web_root = tempfile::tempdir()?;
    fs::write(web_root.path().join("index.html"), "<!doctype html>")?;

    // Hold the configured port without serving HTTP. The MCP handshake and
    // tool listing must still succeed while the Host supervisor retries.
    let blocker = TcpListener::bind("127.0.0.1:0")?;
    let port = blocker.local_addr()?.port();
    let transport = TokioChildProcess::new(
        tokio::process::Command::new(env!("CARGO_BIN_EXE_linkr-host")).configure(|command| {
            command
                .arg("mcp")
                .arg("--port")
                .arg(port.to_string())
                .arg("--web-root")
                .arg(web_root.path())
                .arg("--board-url")
                .arg("http://127.0.0.1:1");
        }),
    )?;

    let client = tokio::time::timeout(Duration::from_secs(5), ().serve(transport)).await??;
    assert_tool_names(&client.list_all_tools().await?);
    drop(blocker);
    wait_for_host_health(port).await?;

    client.cancel().await?;
    Ok(())
}

fn unused_port() -> Result<u16> {
    let listener = TcpListener::bind("127.0.0.1:0")?;
    let port = listener.local_addr()?.port();
    drop(listener);
    Ok(port)
}

async fn wait_for_host_health(port: u16) -> Result<()> {
    let client = reqwest::Client::builder()
        .timeout(Duration::from_millis(500))
        .build()?;
    let url = format!("http://127.0.0.1:{port}/healthz");
    for _ in 0..80 {
        if let Ok(response) = client.get(&url).send().await {
            if response.status().is_success() {
                let value = response.json::<serde_json::Value>().await?;
                if value.get("schema").and_then(|value| value.as_str())
                    == Some("radxa-linkr-debugger-gateway.v1")
                {
                    return Ok(());
                }
            }
        }
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
    bail!("managed Host did not recover on {url}")
}

fn assert_tool_names(tools: &[rmcp::model::Tool]) {
    let mut names = tools
        .iter()
        .map(|tool| tool.name.to_string())
        .collect::<Vec<_>>();
    names.sort();
    assert_eq!(
        names,
        [
            "linkr_adc_read",
            "linkr_board_status",
            "linkr_power_set",
            "linkr_serial_command",
            "linkr_serial_connect",
            "linkr_serial_disconnect",
            "linkr_serial_expect",
            "linkr_serial_login",
            "linkr_serial_read",
            "linkr_serial_shell_command",
            "linkr_serial_status",
            "linkr_serial_write",
            "linkr_switch_route",
        ]
    );
}

#[derive(Clone, Default)]
struct ProgressClient {
    notifications: Arc<AtomicUsize>,
}

impl ClientHandler for ProgressClient {
    async fn on_progress(
        &self,
        _params: ProgressNotificationParam,
        _context: rmcp::service::NotificationContext<rmcp::RoleClient>,
    ) {
        self.notifications.fetch_add(1, Ordering::SeqCst);
    }
}
