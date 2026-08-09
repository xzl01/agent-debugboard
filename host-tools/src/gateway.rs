use std::{sync::Arc, time::Duration};

use anyhow::{Context, Result};
use axum::{
    body::{to_bytes, Body},
    extract::{
        ws::{CloseFrame, Message as AxumMessage, WebSocket, WebSocketUpgrade},
        OriginalUri, Request, State,
    },
    http::{header, HeaderName, HeaderValue, Method, StatusCode},
    middleware::{self, Next},
    response::{IntoResponse, Response},
    routing::{any, get},
    Json, Router,
};
use futures_util::{SinkExt, StreamExt};
use reqwest::Client;
use serde_json::{json, Value};
use tokio::net::TcpListener;
use tokio_tungstenite::{connect_async, tungstenite::Message as TungsteniteMessage};
use tower_http::{
    services::{ServeDir, ServeFile},
    trace::TraceLayer,
};
use tracing::{info, warn};
use url::Url;

use crate::{
    config::{is_allowed_origin, HostConfig},
    serial_broker::{SerialBroker, PROTOCOL as SERIAL_PROTOCOL},
};

const GATEWAY_SCHEMA: &str = "radxa-linkr-debugger-gateway.v1";
const MAX_PROXY_BODY: usize = 16 * 1024 * 1024;

#[derive(Clone)]
struct AppState {
    config: Arc<HostConfig>,
    client: Client,
    serial: SerialBroker,
}

pub async fn serve(config: HostConfig) -> Result<()> {
    let client = Client::builder()
        .pool_max_idle_per_host(1)
        .pool_idle_timeout(Duration::from_secs(90))
        .connect_timeout(Duration::from_secs(5))
        .build()
        .context("build board HTTP client")?;
    let serial = SerialBroker::new(config.serial_idle_timeout);
    let state = AppState {
        config: Arc::new(config),
        client,
        serial,
    };
    let index = state.config.web_root.join("index.html");
    let static_files = ServeDir::new(&state.config.web_root)
        .append_index_html_on_directories(true)
        .not_found_service(ServeFile::new(index));

    let app = Router::new()
        .route("/healthz", get(health))
        .route("/host/api/v1/status", get(host_status))
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
            let _ = tokio::signal::ctrl_c().await;
            shutdown_serial.shutdown().await;
        })
        .await
        .context("run Linkr Host server")
}

async fn health() -> Json<Value> {
    Json(json!({
        "schema": GATEWAY_SCHEMA,
        "ok": true,
        "service": "linkr-host",
        "serial_protocol": SERIAL_PROTOCOL,
        "version": env!("CARGO_PKG_VERSION")
    }))
}

async fn host_status(State(state): State<AppState>) -> Json<Value> {
    Json(json!({
        "schema": "radxa-linkr-host.v1",
        "ok": true,
        "version": env!("CARGO_PKG_VERSION"),
        "pid": std::process::id(),
        "listen": state.config.public_url(),
        "board_url": state.config.board_url.as_str(),
        "web_root": state.config.web_root.display().to_string(),
        "serial_protocol": SERIAL_PROTOCOL
    }))
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
    ws.on_upgrade(move |socket| proxy_websocket(socket, target))
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

async fn proxy_websocket(client: WebSocket, target: Url) {
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

    loop {
        tokio::select! {
            message = client_stream.next() => match message {
                Some(Ok(message)) => {
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

fn board_ws_url(board: &Url, path: &str) -> Result<Url, url::ParseError> {
    let mut target = board.join(path)?;
    match target.scheme() {
        "http" => target.set_scheme("ws").expect("ws is a valid replacement"),
        "https" => target
            .set_scheme("wss")
            .expect("wss is a valid replacement"),
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
}
