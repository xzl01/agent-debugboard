// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::persistent_config::{
    ConfigAction, ConfigApplyRequest, ConfigItemId, ConfigSaveRequest, PersistentConfigResponse,
};
use anyhow::{bail, Context, Result};
use reqwest::blocking::{Body, Client};
use reqwest::{Method, Url};
use serde_json::Value;
use std::fs::File;
use std::time::Duration;

pub const DEFAULT_BASE_URL: &str = "http://172.29.203.1";
const MIN_OTA_UPLOAD_TIMEOUT: Duration = Duration::from_secs(60);

fn ota_upload_timeout(timeout: Duration) -> Duration {
    timeout.max(MIN_OTA_UPLOAD_TIMEOUT)
}

#[derive(Debug, Clone)]
pub struct BoardClient {
    base_url: String,
    http: Client,
    timeout: Duration,
}

pub trait BoardTransport {
    fn send_text(&self, request: BoardRequest) -> Result<String>;
    fn upload_binary(&self, request: BoardBinaryUpload) -> Result<String>;
    fn base_url(&self) -> &str;

    fn config_show(&self) -> Result<PersistentConfigResponse> {
        config_response(self.send_text(config_show_request())?, &ConfigAction::Get)
    }

    fn config_save(
        &self,
        items: &[ConfigItemId],
        confirm: bool,
    ) -> Result<PersistentConfigResponse> {
        config_response(
            self.send_text(config_save_request(items, confirm)?)?,
            &ConfigAction::Save,
        )
    }

    fn config_apply(&self, confirm: bool) -> Result<PersistentConfigResponse> {
        config_response(
            self.send_text(config_apply_request(confirm)?)?,
            &ConfigAction::Apply,
        )
    }

    fn config_clear(&self) -> Result<PersistentConfigResponse> {
        config_response(
            self.send_text(config_clear_request())?,
            &ConfigAction::Clear,
        )
    }
}

#[derive(Debug, Clone)]
pub struct BoardRequest {
    pub method: Method,
    pub path: String,
    pub query: Vec<(String, String)>,
    pub body: Option<Value>,
}

#[derive(Debug)]
pub struct BoardBinaryUpload {
    pub path: String,
    pub file: File,
    pub size: u64,
    pub sha256: String,
}

impl BoardClient {
    pub fn new(base_url: &str, timeout: Duration) -> Result<Self> {
        let base_url = resolve_base_url(base_url);
        let http = Client::builder().timeout(timeout).build()?;
        Ok(Self {
            base_url,
            http,
            timeout,
        })
    }

    pub fn base_url(&self) -> &str {
        &self.base_url
    }

    pub fn send_text(&self, request: BoardRequest) -> Result<String> {
        let url = self.request_url(&request.path, &request.query)?;
        let mut builder = self
            .http
            .request(request.method, url)
            .header("Accept", "application/json");
        if let Some(body) = request.body {
            builder = builder
                .header("Content-Type", "application/json")
                .json(&body);
        }

        Self::response_text(builder.send()?)
    }

    pub fn upload_binary(&self, request: BoardBinaryUpload) -> Result<String> {
        let url = self.request_url(&request.path, &[])?;
        let response = self
            .http
            .post(url)
            .header("Accept", "application/json")
            .header("Content-Type", "application/octet-stream")
            .header("X-Linkr-Ota-Size", request.size.to_string())
            .header("X-Linkr-Ota-Sha256", request.sha256)
            .body(Body::sized(request.file, request.size))
            .timeout(ota_upload_timeout(self.timeout))
            .send()?;
        Self::response_body(response)
    }

    pub fn config_show(&self) -> Result<PersistentConfigResponse> {
        self.send_config(config_show_request(), ConfigAction::Get)
    }

    pub fn config_save(
        &self,
        items: &[ConfigItemId],
        confirm: bool,
    ) -> Result<PersistentConfigResponse> {
        self.send_config(config_save_request(items, confirm)?, ConfigAction::Save)
    }

    pub fn config_apply(&self, confirm: bool) -> Result<PersistentConfigResponse> {
        self.send_config(config_apply_request(confirm)?, ConfigAction::Apply)
    }

    pub fn config_clear(&self) -> Result<PersistentConfigResponse> {
        self.send_config(config_clear_request(), ConfigAction::Clear)
    }

    fn request_url(&self, path: &str, query: &[(String, String)]) -> Result<Url> {
        let mut url = Url::parse(&self.base_url)
            .with_context(|| format!("parse base URL {:?}", self.base_url))?;
        let joined_path = join_request_path(url.path(), path);
        url.set_path(&joined_path);
        if !query.is_empty() {
            let mut pairs = url.query_pairs_mut();
            for (key, value) in query {
                pairs.append_pair(key, value);
            }
        }
        Ok(url)
    }

    fn send_config(
        &self,
        request: BoardRequest,
        expected: ConfigAction,
    ) -> Result<PersistentConfigResponse> {
        let url = self.request_url(&request.path, &request.query)?;
        let mut builder = self
            .http
            .request(request.method, url)
            .header("Accept", "application/json");
        if let Some(body) = request.body {
            builder = builder
                .header("Content-Type", "application/json")
                .json(&body);
        }
        let response = builder.send()?;
        let status = response.status();
        let body = Self::response_body(response)?;
        let response = PersistentConfigResponse::from_raw(body)
            .with_context(|| format!("decode config response from HTTP {status}"))?;
        response.validate(&expected, Some(status.as_u16()))?;
        Ok(response)
    }

    fn response_text(response: reqwest::blocking::Response) -> Result<String> {
        let status = response.status();
        let text = Self::response_body(response)?;
        if !status.is_success() {
            let message = text.trim();
            let detail = if message.is_empty() {
                status.to_string()
            } else {
                message.to_string()
            };
            bail!("HTTP {status}: {detail}");
        }

        Ok(text)
    }

    fn response_body(response: reqwest::blocking::Response) -> Result<String> {
        response.text().context("read response body")
    }
}

fn join_request_path(base_path: &str, request_path: &str) -> String {
    let base = base_path.trim_end_matches('/');
    let req = request_path.trim_start_matches('/');
    if base.is_empty() {
        return format!("/{req}");
    }
    format!("{base}/{req}")
}

impl BoardTransport for BoardClient {
    fn send_text(&self, request: BoardRequest) -> Result<String> {
        Self::send_text(self, request)
    }

    fn upload_binary(&self, request: BoardBinaryUpload) -> Result<String> {
        Self::upload_binary(self, request)
    }

    fn base_url(&self) -> &str {
        Self::base_url(self)
    }

    fn config_show(&self) -> Result<PersistentConfigResponse> {
        Self::config_show(self)
    }

    fn config_save(
        &self,
        items: &[ConfigItemId],
        confirm: bool,
    ) -> Result<PersistentConfigResponse> {
        Self::config_save(self, items, confirm)
    }

    fn config_apply(&self, confirm: bool) -> Result<PersistentConfigResponse> {
        Self::config_apply(self, confirm)
    }

    fn config_clear(&self) -> Result<PersistentConfigResponse> {
        Self::config_clear(self)
    }
}

fn config_show_request() -> BoardRequest {
    BoardRequest {
        method: Method::GET,
        path: "/api/v1/config".to_string(),
        query: vec![],
        body: None,
    }
}

fn config_save_request(items: &[ConfigItemId], confirm: bool) -> Result<BoardRequest> {
    Ok(BoardRequest {
        method: Method::PUT,
        path: "/api/v1/config".to_string(),
        query: vec![],
        body: Some(serde_json::to_value(ConfigSaveRequest { items, confirm })?),
    })
}

fn config_apply_request(confirm: bool) -> Result<BoardRequest> {
    Ok(BoardRequest {
        method: Method::POST,
        path: "/api/v1/config/apply".to_string(),
        query: vec![],
        body: Some(serde_json::to_value(ConfigApplyRequest { confirm })?),
    })
}

fn config_clear_request() -> BoardRequest {
    BoardRequest {
        method: Method::DELETE,
        path: "/api/v1/config".to_string(),
        query: vec![],
        body: None,
    }
}

fn config_response(raw_json: String, expected: &ConfigAction) -> Result<PersistentConfigResponse> {
    let response = PersistentConfigResponse::from_raw(raw_json)?;
    response.validate(expected, None)?;
    Ok(response)
}

pub fn resolve_base_url(input: &str) -> String {
    let trimmed = input.trim();
    if trimmed.is_empty() {
        return DEFAULT_BASE_URL.to_string();
    }
    if trimmed.starts_with("http://") || trimmed.starts_with("https://") {
        return trimmed.trim_end_matches('/').to_string();
    }
    format!("http://{}", trimmed.trim_end_matches('/'))
}

#[cfg(test)]
mod tests {
    use super::{
        join_request_path, ota_upload_timeout, resolve_base_url, BoardBinaryUpload, BoardClient,
        BoardRequest, DEFAULT_BASE_URL,
    };
    use reqwest::Method;
    use std::io::{Read, Write};
    use std::time::Duration;

    #[test]
    fn uses_default_url() {
        assert_eq!(resolve_base_url(""), DEFAULT_BASE_URL);
    }

    #[test]
    fn normalizes_host_port() {
        assert_eq!(
            resolve_base_url("172.29.203.1:9090/"),
            "http://172.29.203.1:9090"
        );
    }

    #[test]
    fn joins_request_path_like_go() {
        assert_eq!(
            join_request_path("/prefix", "/api/v1/status"),
            "/prefix/api/v1/status"
        );
        assert_eq!(join_request_path("", "/api/v1/status"), "/api/v1/status");
    }

    #[test]
    fn ota_upload_timeout_has_a_sixty_second_floor() {
        assert_eq!(
            ota_upload_timeout(Duration::from_secs(2)),
            Duration::from_secs(60)
        );
        assert_eq!(
            ota_upload_timeout(Duration::from_secs(90)),
            Duration::from_secs(90)
        );
    }

    #[test]
    fn preserves_base_path_when_sending_request() {
        let server = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = server.local_addr().unwrap();
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::spawn(move || {
            let (mut stream, _) = server.accept().unwrap();
            let mut buf = [0u8; 2048];
            let n = std::io::Read::read(&mut stream, &mut buf).unwrap();
            let request = String::from_utf8_lossy(&buf[..n]).to_string();
            tx.send(request).unwrap();
            let response = b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
            std::io::Write::write_all(&mut stream, response).unwrap();
        });

        let client =
            BoardClient::new(&format!("http://{}/prefix", addr), Duration::from_secs(2)).unwrap();
        let _ = client
            .send_text(BoardRequest {
                method: Method::GET,
                path: "/api/v1/status".to_string(),
                query: vec![],
                body: None,
            })
            .unwrap();

        let raw = rx.recv().unwrap();
        assert!(
            raw.starts_with("GET /prefix/api/v1/status HTTP/1.1"),
            "{raw}"
        );
    }

    #[test]
    fn upload_binary_sends_streamed_body_with_headers() {
        let server = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = server.local_addr().unwrap();
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::spawn(move || {
            let (mut stream, _) = server.accept().unwrap();
            let request = read_full_http_request(&mut stream);
            tx.send(request).unwrap();
            let body = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"ota","action":"upload"}"#;
            let response = format!(
                "HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n{}",
                body.len(),
                body
            );
            stream.write_all(response.as_bytes()).unwrap();
        });

        let path = temp_file_path("ota-upload-body");
        std::fs::write(&path, b"abc123").unwrap();
        let file = std::fs::File::open(&path).unwrap();
        let client =
            BoardClient::new(&format!("http://{}/prefix", addr), Duration::from_secs(2)).unwrap();
        let output = client
            .upload_binary(BoardBinaryUpload {
                path: "/api/v1/ota/upload".to_string(),
                file,
                size: 6,
                sha256: "6ca13d52ca70c883e0f0bb101e425a89e8624de51db2d2392593af6a84118090"
                    .to_string(),
            })
            .unwrap();
        let _ = std::fs::remove_file(&path);

        assert!(output.contains(r#""command":"ota""#));
        let raw = rx.recv().unwrap();
        assert!(
            raw.starts_with("POST /prefix/api/v1/ota/upload HTTP/1.1"),
            "{raw}"
        );
        assert_header(&raw, "content-type", "application/octet-stream");
        assert_header(&raw, "accept", "application/json");
        assert_header(&raw, "content-length", "6");
        assert_header(&raw, "x-linkr-ota-size", "6");
        assert_header(
            &raw,
            "x-linkr-ota-sha256",
            "6ca13d52ca70c883e0f0bb101e425a89e8624de51db2d2392593af6a84118090",
        );
        assert!(raw.ends_with("\r\n\r\nabc123"), "{raw}");
    }

    #[test]
    fn upload_binary_preserves_non_success_json_body() {
        let server = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = server.local_addr().unwrap();
        std::thread::spawn(move || {
            let (mut stream, _) = server.accept().unwrap();
            let _ = read_full_http_request(&mut stream);
            let body = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"ota","error":{"code":"image_too_large","message":"OTA upload failed validation"}}"#;
            let response = format!(
                "HTTP/1.1 413 Payload Too Large\r\nContent-Length: {}\r\n\r\n{}",
                body.len(),
                body
            );
            stream.write_all(response.as_bytes()).unwrap();
        });

        let path = temp_file_path("ota-upload-error-body");
        std::fs::write(&path, b"abc123").unwrap();
        let file = std::fs::File::open(&path).unwrap();
        let client = BoardClient::new(&format!("http://{}", addr), Duration::from_secs(2)).unwrap();
        let output = client
            .upload_binary(BoardBinaryUpload {
                path: "/api/v1/ota/upload".to_string(),
                file,
                size: 6,
                sha256: "6ca13d52ca70c883e0f0bb101e425a89e8624de51db2d2392593af6a84118090"
                    .to_string(),
            })
            .unwrap();
        let _ = std::fs::remove_file(&path);

        assert!(output.contains(r#""code":"image_too_large""#));
    }

    fn read_full_http_request(stream: &mut std::net::TcpStream) -> String {
        let mut data = Vec::new();
        let mut buf = [0u8; 1024];
        let mut content_length = None;
        loop {
            let n = stream.read(&mut buf).unwrap();
            assert_ne!(n, 0, "connection closed before full request");
            data.extend_from_slice(&buf[..n]);
            if content_length.is_none() {
                if let Some(header_end) = find_header_end(&data) {
                    let headers = String::from_utf8_lossy(&data[..header_end]);
                    content_length = header_value(&headers, "content-length")
                        .and_then(|value| value.parse::<usize>().ok());
                }
            }
            if let (Some(header_end), Some(len)) = (find_header_end(&data), content_length) {
                if data.len() >= header_end + 4 + len {
                    break;
                }
            }
        }
        String::from_utf8_lossy(&data).to_string()
    }

    fn find_header_end(data: &[u8]) -> Option<usize> {
        data.windows(4).position(|window| window == b"\r\n\r\n")
    }

    fn header_value<'a>(headers: &'a str, name: &str) -> Option<&'a str> {
        headers.lines().find_map(|line| {
            let (key, value) = line.split_once(':')?;
            if key.eq_ignore_ascii_case(name) {
                Some(value.trim())
            } else {
                None
            }
        })
    }

    fn assert_header(raw: &str, name: &str, expected: &str) {
        let headers = raw.split("\r\n\r\n").next().unwrap();
        assert_eq!(header_value(headers, name), Some(expected), "{raw}");
    }

    fn temp_file_path(name: &str) -> std::path::PathBuf {
        let mut path = std::env::temp_dir();
        path.push(format!(
            "radxa-linkr-debugger-client-test-{name}-{}",
            std::process::id(),
        ));
        path
    }
}
