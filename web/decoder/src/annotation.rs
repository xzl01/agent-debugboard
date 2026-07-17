// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct Annotation {
    pub start_sample: usize,
    pub end_sample: usize,
    pub row: String,
    pub class: String,
    pub short_text: String,
    pub long_text: String,
    pub data: Value,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct Diagnostic {
    pub start_sample: usize,
    pub end_sample: usize,
    pub severity: DiagnosticSeverity,
    pub code: String,
    pub message: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum DiagnosticSeverity {
    Info,
    Warning,
    Error,
}

pub(crate) fn annotation(
    start_sample: usize,
    end_sample: usize,
    row: &str,
    class: &str,
    short_text: String,
    long_text: String,
    data: Value,
) -> Annotation {
    Annotation {
        start_sample,
        end_sample: end_sample.max(start_sample + 1),
        row: row.to_string(),
        class: class.to_string(),
        short_text,
        long_text,
        data,
    }
}

pub(crate) fn diagnostic(
    start_sample: usize,
    end_sample: usize,
    severity: DiagnosticSeverity,
    code: &str,
    message: &str,
) -> Diagnostic {
    Diagnostic {
        start_sample,
        end_sample: end_sample.max(start_sample + 1),
        severity,
        code: code.to_string(),
        message: message.to_string(),
    }
}
