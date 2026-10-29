// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::client_url::join_request_path;
pub use crate::client_url::resolve_base_url;
#[cfg(test)]
pub use crate::client_url::DEFAULT_BASE_URL;
use crate::persistent_config::{
    ConfigAction, ConfigItemId, ConfigSaveRequest, PersistentConfigResponse,
};
use anyhow::{Context, Result};
use reqwest::blocking::{Body, Client};
use reqwest::header::CONTENT_TYPE;
use reqwest::{Method, StatusCode, Url};
use serde_json::Value;
use std::fmt;
use std::fs::File;
use std::net::{IpAddr, Ipv4Addr};
use std::time::Duration;

const MIN_OTA_UPLOAD_TIMEOUT: Duration = Duration::from_secs(60);

/// Preserve how the target was selected whenever a worker rebuilds a client.
/// Configured URLs retain reqwest defaults; discovered peers stay direct.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BoardTarget {
    pub base_url: String,
    pub local_ip: Option<Ipv4Addr>,
}

impl BoardTarget {
    pub fn direct(base_url: String, local_ip: Ipv4Addr) -> Self {
        Self {
            base_url,
            local_ip: Some(local_ip),
        }
    }
}

impl From<String> for BoardTarget {
    fn from(base_url: String) -> Self {
        Self {
            base_url,
            local_ip: None,
        }
    }
}

impl From<&str> for BoardTarget {
    fn from(base_url: &str) -> Self {
        Self::from(base_url.to_string())
    }
}

impl From<&String> for BoardTarget {
    fn from(base_url: &String) -> Self {
        Self::from(base_url.as_str())
    }
}

impl From<&BoardTarget> for BoardTarget {
    fn from(target: &BoardTarget) -> Self {
        target.clone()
    }
}

pub(crate) fn ota_upload_timeout(timeout: Duration) -> Duration {
    timeout.max(MIN_OTA_UPLOAD_TIMEOUT)
}

#[derive(Debug, Clone)]
pub struct BoardClient {
    target: BoardTarget,
    http: Client,
    timeout: Duration,
}

/// A rejected HTTP response, retaining its body independently of the display text.
#[derive(Debug)]
pub struct BoardHttpError {
    pub status: StatusCode,
    pub body: String,
}

impl fmt::Display for BoardHttpError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let detail = self.body.trim();
        if detail.is_empty() {
            write!(f, "HTTP {}: {}", self.status, self.status)
        } else {
            write!(f, "HTTP {}: {detail}", self.status)
        }
    }
}

impl std::error::Error for BoardHttpError {}

pub trait BoardTransport {
    fn send_text(&self, request: BoardRequest) -> Result<String>;
    fn upload_binary(&self, request: BoardBinaryUpload) -> Result<String>;
    fn send_bytes(&self, _request: BoardBytesRequest) -> Result<BoardBytesResponse> {
        anyhow::bail!("byte-oriented requests are not supported by this transport")
    }
    fn base_url(&self) -> &str;

    fn target(&self) -> BoardTarget {
        BoardTarget::from(self.base_url())
    }

    fn send_reboot(&self) -> Result<Option<String>> {
        self.send_text(reboot_request()).map(Some)
    }

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

#[derive(Debug, Clone)]
pub struct BoardBytesRequest {
    pub method: Method,
    pub path: String,
    pub query: Vec<(String, String)>,
    pub accept: &'static str,
    pub content_type: Option<&'static str>,
    pub body: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct BoardBytesResponse {
    pub status: StatusCode,
    pub content_type: Option<String>,
    pub body: Vec<u8>,
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
        let target = BoardTarget::from(resolve_base_url(base_url));
        let http = Client::builder().timeout(timeout).build()?;
        Ok(Self {
            target,
            http,
            timeout,
        })
    }

    pub fn from_target(target: impl Into<BoardTarget>, timeout: Duration) -> Result<Self> {
        let mut target = target.into();
        let Some(local_ip) = target.local_ip else {
            return Self::new(&target.base_url, timeout);
        };
        target.base_url = resolve_base_url(&target.base_url);
        let http = Client::builder()
            .timeout(timeout)
            .no_proxy()
            .redirect(reqwest::redirect::Policy::none())
            .local_address(IpAddr::V4(local_ip))
            .build()?;
        Ok(Self {
            target,
            http,
            timeout,
        })
    }

    fn request_url(&self, path: &str, query: &[(String, String)]) -> Result<Url> {
        let mut url = Url::parse(&self.target.base_url)
            .with_context(|| format!("parse base URL {:?}", self.target.base_url))?;
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

    fn send_request(&self, request: BoardRequest) -> Result<reqwest::blocking::Response> {
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
        Ok(builder.send()?)
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
            return Err(BoardHttpError { status, body: text }.into());
        }

        Ok(text)
    }

    fn response_body(response: reqwest::blocking::Response) -> Result<String> {
        response.text().context("read response body")
    }
}

impl BoardTransport for BoardClient {
    fn send_text(&self, request: BoardRequest) -> Result<String> {
        Self::response_text(self.send_request(request)?)
    }

    fn upload_binary(&self, request: BoardBinaryUpload) -> Result<String> {
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

    fn send_bytes(&self, request: BoardBytesRequest) -> Result<BoardBytesResponse> {
        let url = self.request_url(&request.path, &request.query)?;
        let mut builder = self
            .http
            .request(request.method, url)
            .header("Accept", request.accept);
        if let Some(content_type) = request.content_type {
            builder = builder.header("Content-Type", content_type);
        }
        if !request.body.is_empty() {
            builder = builder.body(request.body);
        }
        let response = builder.send()?;
        let status = response.status();
        let content_type = response
            .headers()
            .get(CONTENT_TYPE)
            .and_then(|value| value.to_str().ok())
            .map(str::to_owned);
        let body = response.bytes().context("read response body")?.to_vec();
        Ok(BoardBytesResponse {
            status,
            content_type,
            body,
        })
    }

    fn base_url(&self) -> &str {
        &self.target.base_url
    }

    fn target(&self) -> BoardTarget {
        self.target.clone()
    }

    fn send_reboot(&self) -> Result<Option<String>> {
        let response = self.send_request(reboot_request())?;
        if response.status() == StatusCode::ACCEPTED {
            return Ok(None);
        }
        Self::response_text(response).map(Some)
    }

    fn config_show(&self) -> Result<PersistentConfigResponse> {
        self.send_config(config_show_request(), ConfigAction::Get)
    }

    fn config_save(
        &self,
        items: &[ConfigItemId],
        confirm: bool,
    ) -> Result<PersistentConfigResponse> {
        self.send_config(config_save_request(items, confirm)?, ConfigAction::Save)
    }

    fn config_clear(&self) -> Result<PersistentConfigResponse> {
        self.send_config(config_clear_request(), ConfigAction::Clear)
    }
}

pub(crate) fn reboot_request() -> BoardRequest {
    BoardRequest {
        method: Method::POST,
        path: "/api/v1/reboot".to_string(),
        query: vec![],
        body: None,
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
