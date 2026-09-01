use std::{
    fs::File,
    io::{self, Read},
    path::PathBuf,
    sync::Arc,
    time::Duration,
};

use anyhow::{Context, Result};
use axum::{
    body::{to_bytes, Body},
    extract::{
        ws::{CloseFrame, Message as AxumMessage, WebSocket, WebSocketUpgrade},
        OriginalUri, Path, Query, Request, State,
    },
    http::{header, HeaderMap, HeaderName, HeaderValue, Method, StatusCode},
    middleware::{self, Next},
    response::{IntoResponse, Response},
    routing::{any, get, post, put},
    Json, Router,
};
use futures_util::{SinkExt, StreamExt, TryStreamExt};
use reqwest::Client;
use rmcp::transport::streamable_http_server::{
    session::local::LocalSessionManager, StreamableHttpServerConfig, StreamableHttpService,
};
use serde::Deserialize;
use serde_json::{json, Value};
use tokio::{net::TcpListener, sync::Notify};
use tokio_tungstenite::{connect_async, tungstenite::Message as TungsteniteMessage};
use tokio_util::io::ReaderStream;
use tower_http::{
    services::{ServeDir, ServeFile},
    trace::TraceLayer,
};
use tracing::{info, warn};
use url::Url;

use crate::{
    config::{is_allowed_origin, HostConfig},
    host_activity::{HostActivity, LogicAnalyzerSession},
    mcp::LinkrMcpServer,
    serial_broker::{SerialBroker, PROTOCOL as SERIAL_PROTOCOL},
    serial_log::SerialLogService,
};

const GATEWAY_SCHEMA: &str = "radxa-linkr-debugger-gateway.v1";
const MAX_PROXY_BODY: usize = 16 * 1024 * 1024;

#[derive(Clone)]
struct AppState {
    config: Arc<HostConfig>,
    client: Client,
    serial: SerialBroker,
    serial_log: SerialLogService,
    activity: HostActivity,
    shutdown: Arc<Notify>,
}

// allow: SIZE_OK — one Axum gateway boundary owns local routes and board proxying.
pub async fn serve(config: HostConfig) -> Result<()> {
    serve_with_shutdown(config, async {
        let _ = tokio::signal::ctrl_c().await;
    })
    .await
}

/// Runs a Host owned by another in-process lifecycle controller.
pub async fn serve_managed(config: HostConfig) -> Result<()> {
    serve_with_shutdown(config, std::future::pending()).await
}

/// Runs a Host until its owner-provided shutdown signal resolves.
pub async fn serve_with_shutdown(
    config: HostConfig,
    shutdown_signal: impl std::future::Future<Output = ()> + Send + 'static,
) -> Result<()> {
    let client = Client::builder()
        .pool_max_idle_per_host(1)
        .pool_idle_timeout(Duration::from_secs(90))
        .connect_timeout(Duration::from_secs(5))
        .build()
        .context("build board HTTP client")?;
    let serial_log = SerialLogService::new(config.serial_log.clone()).await?;
    let serial = SerialBroker::new(config.serial_idle_timeout, serial_log.clone());
    let shutdown = Arc::new(Notify::new());
    let activity = HostActivity::default();
    let state = AppState {
        config: Arc::new(config),
        client,
        serial,
        serial_log,
        activity,
        shutdown: shutdown.clone(),
    };
    let index = state.config.web_root.join("index.html");
    let static_files = ServeDir::new(&state.config.web_root)
        .append_index_html_on_directories(true)
        .not_found_service(ServeFile::new(index));
    let api_base = Url::parse(&format!("{}api/v1/", state.config.public_url()))
        .context("build resident MCP API URL")?;
    let serial_url = Url::parse(&format!(
        "ws://{}:{}/serial",
        state.config.bind.ip(),
        state.config.bind.port()
    ))
    .context("build resident MCP Serial Broker URL")?;
    let mcp_service = StreamableHttpService::new(
        move || {
            LinkrMcpServer::new(api_base.clone(), serial_url.clone()).map_err(std::io::Error::other)
        },
        LocalSessionManager::default().into(),
        StreamableHttpServerConfig::default(),
    );

    let app = Router::new()
        .route("/healthz", get(health))
        .route("/host/api/v1/status", get(host_status))
        .route("/host/api/v1/shutdown", post(shutdown_host))
        .route("/host/api/v1/serial-logging/status", get(serial_log_status))
        .route("/host/api/v1/serial-logs", get(list_serial_logs))
        .route(
            "/host/api/v1/serial-logs/{session_id}",
            get(get_serial_log).delete(delete_serial_log),
        )
        .route(
            "/host/api/v1/serial-logs/{session_id}/pin",
            put(pin_serial_log),
        )
        .route(
            "/host/api/v1/serial-logs/{session_id}/download",
            get(download_serial_log),
        )
        .nest_service("/mcp", mcp_service)
        .route("/serial", get(serial_socket))
        .route("/api/v1/ws/{slot}", get(proxy_websocket_request))
        .route("/api/{*path}", any(proxy_http_request))
        .fallback_service(static_files)
        .layer(middleware::from_fn_with_state(state.clone(), origin_guard))
        .layer(TraceLayer::new_for_http())
        .with_state(state.clone());

    let listener = TcpListener::bind(state.config.bind)
        .await
        .with_context(|| format!("bind Linkr Host on {}", state.config.bind))?;
    info!(url = %state.config.public_url(), board = %state.config.board_url, web_root = %state.config.web_root.display(), "Linkr Host ready");

    let shutdown_serial = state.serial.clone();
    axum::serve(listener, app)
        .with_graceful_shutdown(async move {
            tokio::select! {
                _ = shutdown_signal => {}
                _ = shutdown.notified() => {}
            }
            shutdown_serial.shutdown().await;
        })
        .await
        .context("run Linkr Host server")
}

async fn health(State(state): State<AppState>) -> Json<Value> {
    Json(json!({
        "schema": GATEWAY_SCHEMA,
        "ok": true,
        "service": "linkr-host",
        "mcp_endpoint": "/mcp",
        "serial_protocol": SERIAL_PROTOCOL,
        "version": env!("CARGO_PKG_VERSION"),
        "serial_logging": state.serial_log.status(),
        "activity": state.activity.snapshot()
    }))
}

async fn host_status(State(state): State<AppState>) -> Json<Value> {
    Json(json!({
        "schema": "radxa-linkr-host.v1",
        "ok": true,
        "version": env!("CARGO_PKG_VERSION"),
        "pid": std::process::id(),
        "listen": state.config.public_url(),
        "mcp_endpoint": format!("{}mcp", state.config.public_url()),
        "board_url": state.config.board_url.as_str(),
        "web_root": state.config.web_root.display().to_string(),
        "serial_protocol": SERIAL_PROTOCOL,
        "serial_logging": state.serial_log.status(),
        "activity": state.activity.snapshot()
    }))
}

async fn shutdown_host(State(state): State<AppState>, headers: HeaderMap) -> Response {
    let Some(expected) = state.config.shutdown_token.as_deref() else {
        return log_error(
            StatusCode::NOT_FOUND,
            "managed_shutdown_unavailable",
            "this Host is not managed by the tray",
        );
    };
    let supplied = headers
        .get("x-linkr-shutdown-token")
        .and_then(|value| value.to_str().ok());
    if supplied != Some(expected) {
        return log_error(
            StatusCode::FORBIDDEN,
            "invalid_shutdown_token",
            "managed shutdown token is invalid",
        );
    }
    state.shutdown.notify_one();
    StatusCode::ACCEPTED.into_response()
}

async fn serial_log_status(State(state): State<AppState>, headers: HeaderMap) -> Response {
    if let Some(response) = reject_nonlocal_log_request(&headers) {
        return response;
    }
    Json(state.serial_log.status()).into_response()
}

async fn list_serial_logs(State(state): State<AppState>, headers: HeaderMap) -> Response {
    if let Some(response) = reject_nonlocal_log_request(&headers) {
        return response;
    }
    match state.serial_log.list() {
        Ok(logs) => {
            Json(json!({"schema": "linkr-serial-log-list.v1", "logs": logs})).into_response()
        }
        Err(error) => internal_log_error(error),
    }
}

async fn get_serial_log(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(session_id): Path<String>,
) -> Response {
    if let Some(response) = reject_nonlocal_log_request(&headers) {
        return response;
    }
    let Some(session_id) = parse_session_id(&session_id) else {
        return log_error(
            StatusCode::BAD_REQUEST,
            "invalid_session_id",
            "invalid serial log session id",
        );
    };
    match state.serial_log.find(session_id) {
        Ok(Some(log)) => Json(log).into_response(),
        Ok(None) => log_error(
            StatusCode::NOT_FOUND,
            "not_found",
            "serial log session not found",
        ),
        Err(error) => internal_log_error(error),
    }
}

#[derive(Debug, Deserialize)]
struct PinRequest {
    pinned: bool,
}

async fn pin_serial_log(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(session_id): Path<String>,
    Json(request): Json<PinRequest>,
) -> Response {
    if let Some(response) = reject_nonlocal_log_request(&headers) {
        return response;
    }
    let Some(session_id) = parse_session_id(&session_id) else {
        return log_error(
            StatusCode::BAD_REQUEST,
            "invalid_session_id",
            "invalid serial log session id",
        );
    };
    match state.serial_log.set_pinned(session_id, request.pinned) {
        Ok(log) => Json(log).into_response(),
        Err(error) => log_operation_error(error),
    }
}

#[derive(Debug, Deserialize)]
struct DeleteQuery {
    confirm: Option<bool>,
}

async fn delete_serial_log(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(session_id): Path<String>,
    Query(query): Query<DeleteQuery>,
) -> Response {
    if let Some(response) = reject_nonlocal_log_request(&headers) {
        return response;
    }
    if query.confirm != Some(true) {
        return log_error(
            StatusCode::BAD_REQUEST,
            "confirmation_required",
            "delete requires confirm=true",
        );
    }
    let Some(session_id) = parse_session_id(&session_id) else {
        return log_error(
            StatusCode::BAD_REQUEST,
            "invalid_session_id",
            "invalid serial log session id",
        );
    };
    match state.serial_log.delete(session_id) {
        Ok(()) => StatusCode::NO_CONTENT.into_response(),
        Err(error) => log_operation_error(error),
    }
}

#[derive(Debug, Deserialize)]
struct DownloadQuery {
    format: Option<String>,
}

async fn download_serial_log(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(session_id): Path<String>,
    Query(query): Query<DownloadQuery>,
) -> Response {
    if let Some(response) = reject_nonlocal_log_request(&headers) {
        return response;
    }
    let Some(session_id) = parse_session_id(&session_id) else {
        return log_error(
            StatusCode::BAD_REQUEST,
            "invalid_session_id",
            "invalid serial log session id",
        );
    };
    let format = query.format.as_deref().unwrap_or("raw");
    let paths = match state.serial_log.artifact_paths(session_id, format) {
        Ok(paths) => paths,
        Err(error) => return log_operation_error(error),
    };
    let (body, content_type, extension) = match format {
        "raw" => (file_stream_body(paths), "application/octet-stream", "raw"),
        "ndjson" => (file_stream_body(paths), "application/x-ndjson", "ndjson"),
        "text" => (
            lossy_text_stream_body(paths),
            "text/plain; charset=utf-8",
            "txt",
        ),
        _ => {
            return log_error(
                StatusCode::BAD_REQUEST,
                "unsupported_format",
                "unsupported serial log download format",
            )
        }
    };
    let mut response = body.into_response();
    response
        .headers_mut()
        .insert(header::CONTENT_TYPE, HeaderValue::from_static(content_type));
    let disposition = format!("attachment; filename=\"{session_id}.{extension}\"");
    if let Ok(value) = HeaderValue::from_str(&disposition) {
        response
            .headers_mut()
            .insert(header::CONTENT_DISPOSITION, value);
    }
    response
}

fn file_stream_body(paths: Vec<PathBuf>) -> Body {
    let stream = futures_util::stream::iter(paths)
        .map(Ok::<_, io::Error>)
        .and_then(tokio::fs::File::open)
        .map_ok(ReaderStream::new)
        .try_flatten();
    Body::from_stream(stream)
}

fn lossy_text_stream_body(paths: Vec<PathBuf>) -> Body {
    let (sender, receiver) = tokio::sync::mpsc::channel::<io::Result<Vec<u8>>>(4);
    tokio::task::spawn_blocking(move || {
        let mut scratch = vec![0_u8; 64 * 1024];
        let mut pending = Vec::with_capacity(scratch.len() + 4);
        for path in paths {
            let mut file = match File::open(&path) {
                Ok(file) => file,
                Err(error) => {
                    let _ = sender.blocking_send(Err(error));
                    return;
                }
            };
            loop {
                let count = match file.read(&mut scratch) {
                    Ok(0) => break,
                    Ok(count) => count,
                    Err(error) => {
                        let _ = sender.blocking_send(Err(error));
                        return;
                    }
                };
                pending.extend_from_slice(&scratch[..count]);
                let output = drain_lossy_utf8(&mut pending, false);
                if !output.is_empty() && sender.blocking_send(Ok(output)).is_err() {
                    return;
                }
            }
        }
        let output = drain_lossy_utf8(&mut pending, true);
        if !output.is_empty() {
            let _ = sender.blocking_send(Ok(output));
        }
    });
    let stream = futures_util::stream::unfold(receiver, |mut receiver| async move {
        receiver.recv().await.map(|item| (item, receiver))
    });
    Body::from_stream(stream)
}

fn drain_lossy_utf8(pending: &mut Vec<u8>, eof: bool) -> Vec<u8> {
    let mut output = String::new();
    let mut consumed = 0;
    while consumed < pending.len() {
        match std::str::from_utf8(&pending[consumed..]) {
            Ok(text) => {
                output.push_str(text);
                consumed = pending.len();
            }
            Err(error) => {
                let valid = error.valid_up_to();
                if valid > 0 {
                    let valid_text = std::str::from_utf8(&pending[consumed..consumed + valid]);
                    // SAFE-EXPECT: Utf8Error::valid_up_to identifies a valid UTF-8 prefix.
                    output.push_str(valid_text.expect("valid_up_to ends on a UTF-8 boundary"));
                    consumed += valid;
                }
                if let Some(length) = error.error_len() {
                    output.push('\u{fffd}');
                    consumed += length;
                } else if eof {
                    output.push('\u{fffd}');
                    consumed = pending.len();
                } else {
                    break;
                }
            }
        }
    }
    pending.drain(..consumed);
    output.into_bytes()
}

fn parse_session_id(value: &str) -> Option<uuid::Uuid> {
    uuid::Uuid::parse_str(value).ok()
}

fn reject_nonlocal_log_request(headers: &HeaderMap) -> Option<Response> {
    if headers
        .get("sec-fetch-site")
        .and_then(|value| value.to_str().ok())
        .is_some_and(|value| value.eq_ignore_ascii_case("cross-site"))
    {
        return Some(log_error(
            StatusCode::FORBIDDEN,
            "local_origin_required",
            "serial logs are available only to the local Web UI",
        ));
    }
    let origin = headers.get(header::ORIGIN)?;
    let Ok(origin) = origin.to_str() else {
        return Some(log_error(
            StatusCode::FORBIDDEN,
            "local_origin_required",
            "serial logs are available only to the local Web UI",
        ));
    };
    let local = Url::parse(origin).is_ok_and(|url| {
        url.scheme() == "http"
            && url.host().is_some_and(|host| match host {
                url::Host::Domain(name) => name.eq_ignore_ascii_case("localhost"),
                url::Host::Ipv4(address) => address.is_loopback(),
                url::Host::Ipv6(address) => address.is_loopback(),
            })
    });
    if local {
        None
    } else {
        Some(log_error(
            StatusCode::FORBIDDEN,
            "local_origin_required",
            "serial logs are available only to the local Web UI",
        ))
    }
}

fn log_operation_error(error: anyhow::Error) -> Response {
    let message = error.to_string();
    let status = if message.contains("not found") {
        StatusCode::NOT_FOUND
    } else if message.contains("active")
        || message.contains("unpin")
        || message.contains("unsupported")
    {
        StatusCode::CONFLICT
    } else {
        StatusCode::INTERNAL_SERVER_ERROR
    };
    log_error(status, "serial_log_error", &message)
}

fn internal_log_error(error: anyhow::Error) -> Response {
    warn!(error = %error, "serial log API failed");
    log_error(
        StatusCode::INTERNAL_SERVER_ERROR,
        "serial_log_error",
        "serial log operation failed",
    )
}

fn log_error(status: StatusCode, code: &str, message: &str) -> Response {
    (
        status,
        Json(json!({"error": {"code": code, "message": message}})),
    )
        .into_response()
}

async fn serial_socket(State(state): State<AppState>, ws: WebSocketUpgrade) -> Response {
    ws.on_upgrade(move |socket| async move {
        state.serial.serve_socket(socket).await;
    })
}

async fn proxy_websocket_request(
    State(state): State<AppState>,
    OriginalUri(uri): OriginalUri,
    ws: WebSocketUpgrade,
) -> Response {
    let target = match board_ws_url(
        &state.config.board_url,
        uri.path_and_query()
            .map(|value| value.as_str())
            .unwrap_or(uri.path()),
    ) {
        Ok(target) => target,
        Err(error) => {
            return gateway_error(
                StatusCode::BAD_GATEWAY,
                "invalid_board_url",
                &error.to_string(),
            )
        }
    };
    ws.on_upgrade(move |socket| proxy_websocket(socket, target, state.activity))
}

async fn proxy_http_request(
    State(state): State<AppState>,
    OriginalUri(uri): OriginalUri,
    request: Request,
) -> Response {
    proxy_http(state, uri, request).await
}

async fn proxy_http(state: AppState, uri: axum::http::Uri, request: Request) -> Response {
    let target = match state.config.board_url.join(
        uri.path_and_query()
            .map(|value| value.as_str())
            .unwrap_or(uri.path()),
    ) {
        Ok(target) => target,
        Err(error) => {
            return gateway_error(
                StatusCode::BAD_GATEWAY,
                "invalid_board_url",
                &error.to_string(),
            )
        }
    };
    let (parts, body) = request.into_parts();
    let body = match to_bytes(body, MAX_PROXY_BODY).await {
        Ok(body) => body,
        Err(error) => {
            return gateway_error(
                StatusCode::PAYLOAD_TOO_LARGE,
                "request_too_large",
                &error.to_string(),
            )
        }
    };
    let mut upstream = state.client.request(parts.method.clone(), target.clone());
    for (name, value) in &parts.headers {
        if is_hop_by_hop(name) || *name == header::HOST || *name == header::ORIGIN {
            continue;
        }
        upstream = upstream.header(name, value);
    }
    if !body.is_empty() {
        upstream = upstream.body(body);
    }
    let response = match upstream.send().await {
        Ok(response) => response,
        Err(error) => {
            return gateway_error(
                StatusCode::BAD_GATEWAY,
                "board_unreachable",
                &format!("Cannot reach {}: {error}", state.config.board_url),
            )
        }
    };
    let status = response.status();
    let headers = response.headers().clone();
    let body = match response.bytes().await {
        Ok(body) => body,
        Err(error) => {
            return gateway_error(
                StatusCode::BAD_GATEWAY,
                "board_response_failed",
                &error.to_string(),
            )
        }
    };
    let mut output = Response::builder().status(status);
    for (name, value) in &headers {
        if !is_hop_by_hop(name) {
            output = output.header(name, value);
        }
    }
    output.body(Body::from(body)).unwrap_or_else(|error| {
        gateway_error(
            StatusCode::INTERNAL_SERVER_ERROR,
            "gateway_internal_error",
            &error.to_string(),
        )
    })
}

async fn proxy_websocket(client: WebSocket, target: Url, activity: HostActivity) {
    let upstream =
        match tokio::time::timeout(Duration::from_secs(10), connect_async(target.as_str())).await {
            Ok(Ok((socket, _))) => socket,
            Ok(Err(error)) => {
                warn!(%target, %error, "board WebSocket unavailable");
                return;
            }
            Err(_) => {
                warn!(%target, "board WebSocket connection timed out");
                return;
            }
        };
    let (mut client_sink, mut client_stream) = client.split();
    let (mut upstream_sink, mut upstream_stream) = upstream.split();
    let mut logic_analyzer_session: Option<LogicAnalyzerSession> = None;

    loop {
        tokio::select! {
            message = client_stream.next() => match message {
                Some(Ok(message)) => {
                    track_logic_analyzer_binary(
                        matches!(message, AxumMessage::Binary(_)),
                        &activity,
                        &mut logic_analyzer_session,
                    );
                    let close = matches!(message, AxumMessage::Close(_));
                    if let Some(message) = axum_to_tungstenite(message) {
                        if upstream_sink.send(message).await.is_err() { break; }
                    }
                    if close { break; }
                }
                _ => break,
            },
            message = upstream_stream.next() => match message {
                Some(Ok(message)) => {
                    track_logic_analyzer_binary(
                        matches!(message, TungsteniteMessage::Binary(_)),
                        &activity,
                        &mut logic_analyzer_session,
                    );
                    let close = matches!(message, TungsteniteMessage::Close(_));
                    if let Some(message) = tungstenite_to_axum(message) {
                        if client_sink.send(message).await.is_err() { break; }
                    }
                    if close { break; }
                }
                _ => break,
            }
        }
    }
    let _ = upstream_sink.close().await;
    let _ = client_sink.close().await;
}

fn track_logic_analyzer_binary(
    binary: bool,
    activity: &HostActivity,
    session: &mut Option<LogicAnalyzerSession>,
) {
    if binary && session.is_none() {
        *session = Some(activity.begin_logic_analyzer());
    }
}

fn board_ws_url(board: &Url, path: &str) -> Result<Url, url::ParseError> {
    let mut target = board.join(path)?;
    match target.scheme() {
        "http" => {
            // SAFE-EXPECT: ws is a valid absolute URL scheme.
            target.set_scheme("ws").expect("ws is a valid replacement")
        }
        "https" => {
            target
                .set_scheme("wss")
                // SAFE-EXPECT: wss is a valid absolute URL scheme.
                .expect("wss is a valid replacement")
        }
        _ => {}
    }
    Ok(target)
}

fn axum_to_tungstenite(message: AxumMessage) -> Option<TungsteniteMessage> {
    match message {
        AxumMessage::Text(text) => Some(TungsteniteMessage::Text(text.to_string().into())),
        AxumMessage::Binary(bytes) => Some(TungsteniteMessage::Binary(bytes)),
        AxumMessage::Ping(bytes) => Some(TungsteniteMessage::Ping(bytes)),
        AxumMessage::Pong(bytes) => Some(TungsteniteMessage::Pong(bytes)),
        AxumMessage::Close(frame) => Some(TungsteniteMessage::Close(frame.map(|frame| {
            tokio_tungstenite::tungstenite::protocol::CloseFrame {
                code: frame.code.into(),
                reason: frame.reason.to_string().into(),
            }
        }))),
    }
}

fn tungstenite_to_axum(message: TungsteniteMessage) -> Option<AxumMessage> {
    match message {
        TungsteniteMessage::Text(text) => Some(AxumMessage::Text(text.to_string().into())),
        TungsteniteMessage::Binary(bytes) => Some(AxumMessage::Binary(bytes)),
        TungsteniteMessage::Ping(bytes) => Some(AxumMessage::Ping(bytes)),
        TungsteniteMessage::Pong(bytes) => Some(AxumMessage::Pong(bytes)),
        TungsteniteMessage::Close(frame) => {
            Some(AxumMessage::Close(frame.map(|frame| CloseFrame {
                code: frame.code.into(),
                reason: frame.reason.to_string().into(),
            })))
        }
        TungsteniteMessage::Frame(_) => None,
    }
}

async fn origin_guard(State(state): State<AppState>, request: Request, next: Next) -> Response {
    let origin = request
        .headers()
        .get(header::ORIGIN)
        .and_then(|value| value.to_str().ok())
        .map(str::to_owned);
    if !is_allowed_origin(origin.as_deref(), &state.config.trusted_origins) {
        return gateway_error(
            StatusCode::FORBIDDEN,
            "origin_not_allowed",
            "origin is not allowed",
        );
    }
    if request.method() == Method::OPTIONS {
        return add_cors_headers(StatusCode::NO_CONTENT.into_response(), origin.as_deref());
    }
    add_cors_headers(next.run(request).await, origin.as_deref())
}

fn add_cors_headers(mut response: Response, origin: Option<&str>) -> Response {
    let headers = response.headers_mut();
    headers.insert(
        header::ACCESS_CONTROL_ALLOW_ORIGIN,
        HeaderValue::from_str(origin.unwrap_or("*")).unwrap_or(HeaderValue::from_static("*")),
    );
    headers.insert(
        header::ACCESS_CONTROL_ALLOW_METHODS,
        HeaderValue::from_static("GET, POST, PUT, DELETE, OPTIONS"),
    );
    headers.insert(
        header::ACCESS_CONTROL_ALLOW_HEADERS,
        HeaderValue::from_static("Content-Type, X-Linkr-Ota-Size, X-Linkr-Ota-Sha256"),
    );
    headers.insert(
        HeaderName::from_static("access-control-allow-private-network"),
        HeaderValue::from_static("true"),
    );
    headers.insert(header::VARY, HeaderValue::from_static("Origin"));
    response
}

fn is_hop_by_hop(name: &HeaderName) -> bool {
    matches!(
        name.as_str().to_ascii_lowercase().as_str(),
        "connection"
            | "keep-alive"
            | "proxy-authenticate"
            | "proxy-authorization"
            | "te"
            | "trailer"
            | "transfer-encoding"
            | "upgrade"
    )
}

fn gateway_error(status: StatusCode, code: &str, message: &str) -> Response {
    (
        status,
        Json(json!({"ok": false, "error": {"code": code, "message": message}})),
    )
        .into_response()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn maps_board_http_urls_to_websocket_urls() {
        let board = Url::parse("http://172.29.203.1:8080").unwrap();
        assert_eq!(
            board_ws_url(&board, "/api/v1/ws/0").unwrap().as_str(),
            "ws://172.29.203.1:8080/api/v1/ws/0"
        );
    }

    #[test]
    fn detects_hop_by_hop_headers() {
        assert!(is_hop_by_hop(&header::CONNECTION));
        assert!(is_hop_by_hop(&header::UPGRADE));
        assert!(!is_hop_by_hop(&header::CONTENT_TYPE));
    }

    #[test]
    fn binary_proxy_traffic_counts_one_session_until_the_guard_drops() {
        let activity = HostActivity::default();
        let mut session = None;
        assert_eq!(activity.snapshot().logic_analyzer_sessions, 0);
        assert!(!activity.snapshot().logic_analyzer_active);

        track_logic_analyzer_binary(false, &activity, &mut session);
        assert_eq!(activity.snapshot().logic_analyzer_sessions, 0);
        track_logic_analyzer_binary(true, &activity, &mut session);
        assert_eq!(activity.snapshot().logic_analyzer_sessions, 1);
        assert!(activity.snapshot().logic_analyzer_active);
        track_logic_analyzer_binary(true, &activity, &mut session);
        assert_eq!(activity.snapshot().logic_analyzer_sessions, 1);
        drop(session);
        assert_eq!(activity.snapshot().logic_analyzer_sessions, 0);
        assert!(activity.snapshot().logic_analyzer_active);
    }

    #[test]
    fn lossy_utf8_stream_preserves_characters_split_across_chunks() {
        let mut pending = vec![b'A', 0xe4, 0xb8];
        assert_eq!(drain_lossy_utf8(&mut pending, false), b"A");
        assert_eq!(pending, vec![0xe4, 0xb8]);

        pending.extend_from_slice(&[0xad, b'B']);
        assert_eq!(drain_lossy_utf8(&mut pending, false), "中B".as_bytes());
        assert!(pending.is_empty());
    }

    #[test]
    fn lossy_utf8_stream_replaces_invalid_and_incomplete_sequences() {
        let mut pending = vec![b'A', 0xff, b'B', 0xe4];
        assert_eq!(
            drain_lossy_utf8(&mut pending, false),
            "A\u{fffd}B".as_bytes()
        );
        assert_eq!(pending, vec![0xe4]);
        assert_eq!(drain_lossy_utf8(&mut pending, true), "\u{fffd}".as_bytes());
        assert!(pending.is_empty());
    }
}
