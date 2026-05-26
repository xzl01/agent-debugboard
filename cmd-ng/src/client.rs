// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use anyhow::{bail, Context, Result};
use reqwest::blocking::Client;
use reqwest::{Method, Url};
use serde_json::Value;
use std::time::Duration;

pub const DEFAULT_BASE_URL: &str = "http://172.29.203.1:8080";

#[derive(Debug, Clone)]
pub struct BoardClient {
    base_url: String,
    http: Client,
}

pub trait BoardTransport {
    fn send_text(&self, request: BoardRequest) -> Result<String>;
    fn base_url(&self) -> &str;
}

#[derive(Debug, Clone)]
pub struct BoardRequest {
    pub method: Method,
    pub path: String,
    pub query: Vec<(String, String)>,
    pub body: Option<Value>,
}

impl BoardClient {
    pub fn new(base_url: &str, timeout: Duration) -> Result<Self> {
        let base_url = resolve_base_url(base_url);
        let http = Client::builder().timeout(timeout).build()?;
        Ok(Self { base_url, http })
    }

    pub fn base_url(&self) -> &str {
        &self.base_url
    }

    pub fn send_text(&self, request: BoardRequest) -> Result<String> {
        let mut url = Url::parse(&self.base_url)
            .with_context(|| format!("parse base URL {:?}", self.base_url))?;
        let joined_path = join_request_path(url.path(), &request.path);
        url.set_path(&joined_path);
        if !request.query.is_empty() {
            let mut pairs = url.query_pairs_mut();
            for (key, value) in &request.query {
                pairs.append_pair(key, value);
            }
        }

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
        let text = response.text().context("read response body")?;
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

    fn base_url(&self) -> &str {
        Self::base_url(self)
    }
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
    use super::{join_request_path, resolve_base_url, BoardClient, BoardRequest, DEFAULT_BASE_URL};
    use reqwest::Method;
    use std::time::Duration;

    #[test]
    fn uses_default_url() {
        assert_eq!(resolve_base_url(""), DEFAULT_BASE_URL);
    }

    #[test]
    fn normalizes_host_port() {
        assert_eq!(
            resolve_base_url("172.29.203.1:8080/"),
            "http://172.29.203.1:8080"
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
}
