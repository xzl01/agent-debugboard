// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::annotation::{Annotation, Diagnostic};
use crate::input::PackedSamples;
use crate::protocols::ProtocolRequest;
use serde::{Deserialize, Serialize};
use thiserror::Error;

pub const SCHEMA_VERSION: &str = "radxa.logic-decoder.request.v1";
pub const RESULT_SCHEMA_VERSION: &str = "radxa.logic-decoder.result.v1";

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct DecodeRequest {
    pub schema_version: String,
    pub samples: PackedSamples,
    pub protocol: ProtocolRequest,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct DecodeResult {
    pub schema_version: String,
    pub annotations: Vec<Annotation>,
    pub diagnostics: Vec<Diagnostic>,
}

impl DecodeResult {
    pub fn new() -> Self {
        Self {
            schema_version: RESULT_SCHEMA_VERSION.to_string(),
            annotations: Vec::new(),
            diagnostics: Vec::new(),
        }
    }
}

impl Default for DecodeResult {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Debug, Error, PartialEq, Eq)]
pub enum DecodeError {
    #[error("unsupported schema version: {0}")]
    UnsupportedSchema(String),
    #[error("invalid samples: {0}")]
    InvalidSamples(String),
    #[error("invalid options: {0}")]
    InvalidOptions(String),
}
