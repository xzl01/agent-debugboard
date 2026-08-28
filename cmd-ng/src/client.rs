// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::client_url::join_request_path;
pub use crate::client_url::{resolve_base_url, DEFAULT_BASE_URL};
use crate::persistent_config::{
    ConfigAction, ConfigItemId, ConfigSaveRequest, PersistentConfigResponse,
};
use anyhow::{bail, Context, Result};
use reqwest::blocking::{Body, Client};
use reqwest::{Method, Url};
use serde_json::Value;
use std::fs::File;
use std::time::Duration;

const MIN_OTA_UPLOAD_TIMEOUT: Duration = Duration::from_secs(60);

pub(crate) fn ota_upload_timeout(timeout: Duration) -> Duration {
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
    fn send_raw_json(&self, request: BoardRawJsonRequest) -> Result<String>;
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
pub struct BoardRawJsonRequest {
    pub method: Method,
    pub path: String,
    pub query: Vec<(String, String)>,
    pub body: String,
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

impl BoardTransport for BoardClient {
    fn send_text(&self, request: BoardRequest) -> Result<String> {
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

    fn send_raw_json(&self, request: BoardRawJsonRequest) -> Result<String> {
        let url = self.request_url(&request.path, &request.query)?;
        let response = self
            .http
            .request(request.method, url)
            .header("Accept", "application/json")
            .header("Content-Type", "application/json")
            .body(request.body)
            .send()?;
        Self::response_text(response)
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

    fn base_url(&self) -> &str {
        &self.base_url
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
