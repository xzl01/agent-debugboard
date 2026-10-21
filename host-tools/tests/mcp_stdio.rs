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
use serde_json::{json, Value};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    sync::mpsc,
};

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
    let tools = client.list_all_tools().await?;
    assert_tool_names(&tools);
    assert_switch_tool_schema(&tools);
    client.cancel().await?;
    Ok(())
}

#[tokio::test]
async fn stdio_switch_tool_forwards_generic_descriptors_through_the_gateway() -> Result<()> {
    let web_root = tempfile::tempdir()?;
    fs::write(web_root.path().join("index.html"), "<!doctype html>")?;
    let (board_url, mut requests, board) = mock_firmware().await?;
    let port = unused_port()?;
    let transport = TokioChildProcess::new(
        tokio::process::Command::new(env!("CARGO_BIN_EXE_linkr-host")).configure(|command| {
            command
                .arg("mcp")
                .arg("--port")
                .arg(port.to_string())
                .arg("--web-root")
                .arg(web_root.path())
                .arg("--board-url")
                .arg(board_url);
        }),
    )?;
    let client = ().serve(transport).await?;
    wait_for_host_health(port).await?;

    for (name, route, path) in [
        ("tf_wp", "writable", "/api/v1/switch/tf_wp"),
        ("tf_wp", "protected", "/api/v1/switch/tf_wp"),
        ("sd", "usb-reader", "/api/v1/switch/sd"),
        ("aux port", "future-route", "/api/v1/switch/aux%20port"),
        ("aux/name", "future-route", "/api/v1/switch/aux%2Fname"),
    ] {
        let result = client.call_tool(switch_request(name, route, true)).await?;
        assert_eq!(result.is_error, Some(false), "{name}={route}: {result:?}");
        let request = requests.recv().await.expect("firmware received switch PUT");
        assert_eq!(request.path, path);
        assert_eq!(request.body, json!({"route": route}));
    }

    let result = client
        .call_tool(switch_request("usb", "target", true))
        .await?;
    assert_eq!(result.is_error, Some(true));
    let structured = result
        .structured_content
        .expect("firmware 404 remains a structured MCP error");
    assert_eq!(
        structured.pointer("/error/code").and_then(Value::as_str),
        Some("not_found")
    );
    assert_eq!(
        structured
            .pointer("/error/details/status")
            .and_then(Value::as_u64),
        Some(404)
    );
    let request = requests.recv().await.expect("firmware received usb PUT");
    assert_eq!(request.path, "/api/v1/switch/usb");
    assert_eq!(request.body, json!({"route": "target"}));

    for (request, expected_code) in [
        (switch_request("", "target", true), "invalid_switch_name"),
        (switch_request(".", "target", true), "invalid_switch_name"),
        (switch_request("..", "target", true), "invalid_switch_name"),
        (switch_request("sd", "", true), "invalid_route"),
        (
            switch_request("sd", "target", false),
            "confirmation_required",
        ),
    ] {
        let result = client.call_tool(request).await?;
        assert_eq!(result.is_error, Some(true));
        assert_eq!(
            result
                .structured_content
                .as_ref()
                .and_then(|value| value.pointer("/error/code"))
                .and_then(Value::as_str),
            Some(expected_code)
        );
    }
    assert!(
        tokio::time::timeout(Duration::from_millis(100), requests.recv())
            .await
            .is_err(),
        "locally rejected switch calls must not reach the gateway"
    );

    client.cancel().await?;
    board.abort();
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

fn assert_switch_tool_schema(tools: &[rmcp::model::Tool]) {
    let tool = tools
        .iter()
        .find(|tool| tool.name == "linkr_switch_route")
        .expect("switch tool is listed");
    assert_eq!(
        tool.description.as_deref(),
        Some("Set a firmware-advertised switch route by name. Use the switch names and route lists from linkr_board_status with detail=full; confirm must be true because a route change can disconnect or electrically mismatch the target.")
    );
    assert_eq!(
        Value::Object(tool.input_schema.as_ref().clone()),
        json!({
            "$schema": "https://json-schema.org/draft/2020-12/schema",
            "properties": {
                "confirm": {"type": "boolean"},
                "name": {"maxLength": 64, "minLength": 1, "type": "string"},
                "route": {"maxLength": 64, "minLength": 1, "type": "string"}
            },
            "required": ["name", "route", "confirm"],
            "type": "object"
        })
    );
}

fn switch_request(name: &str, route: &str, confirm: bool) -> CallToolRequestParams {
    CallToolRequestParams::new("linkr_switch_route").with_arguments(
        json!({"name": name, "route": route, "confirm": confirm})
            .as_object()
            .expect("switch arguments are an object")
            .clone(),
    )
}

#[derive(Debug)]
struct FirmwareRequest {
    path: String,
    body: Value,
}

async fn mock_firmware() -> Result<(
    String,
    mpsc::UnboundedReceiver<FirmwareRequest>,
    tokio::task::JoinHandle<()>,
)> {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await?;
    let address = listener.local_addr()?;
    let (sender, receiver) = mpsc::unbounded_channel();
    let task = tokio::spawn(async move {
        loop {
            let (mut socket, _) = listener.accept().await.expect("accept firmware request");
            let mut request = Vec::new();
            let header_end = loop {
                let mut chunk = [0_u8; 4096];
                let count = socket
                    .read(&mut chunk)
                    .await
                    .expect("read firmware request");
                assert!(count > 0, "firmware request ended before its headers");
                request.extend_from_slice(&chunk[..count]);
                if let Some(offset) = request.windows(4).position(|bytes| bytes == b"\r\n\r\n") {
                    break offset + 4;
                }
            };
            let content_length = {
                let headers = String::from_utf8_lossy(&request[..header_end]);
                headers
                    .lines()
                    .find_map(|line| {
                        let (name, value) = line.split_once(':')?;
                        name.eq_ignore_ascii_case("content-length")
                            .then(|| value.trim().parse::<usize>().expect("valid content length"))
                    })
                    .unwrap_or(0)
            };
            while request.len() < header_end + content_length {
                let mut chunk = [0_u8; 4096];
                let count = socket
                    .read(&mut chunk)
                    .await
                    .expect("read firmware request body");
                assert!(count > 0, "firmware request body ended early");
                request.extend_from_slice(&chunk[..count]);
            }
            let path = String::from_utf8_lossy(&request[..header_end])
                .lines()
                .next()
                .and_then(|line| line.split_whitespace().nth(1))
                .expect("firmware request path")
                .to_owned();
            let body = serde_json::from_slice(&request[header_end..header_end + content_length])
                .expect("firmware request JSON");
            sender
                .send(FirmwareRequest {
                    path: path.clone(),
                    body,
                })
                .expect("test receives firmware request");

            let (status, body) = if path == "/api/v1/switch/usb" {
                (
                    "404 Not Found",
                    json!({
                        "ok": false,
                        "error": {
                            "code": "not_found",
                            "message": "unknown switch"
                        }
                    }),
                )
            } else {
                (
                    "200 OK",
                    json!({
                        "schema": "radxa-linkr-debugger.v1",
                        "ok": true,
                        "command": "switch"
                    }),
                )
            };
            let body = body.to_string();
            socket
                .write_all(
                    format!(
                        "HTTP/1.1 {status}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
                        body.len()
                    )
                    .as_bytes(),
                )
                .await
                .expect("write firmware response");
        }
    });
    Ok((format!("http://{address}"), receiver, task))
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
