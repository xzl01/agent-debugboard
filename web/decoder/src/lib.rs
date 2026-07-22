// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

//! Project-owned sample-level UART, I2C, and SPI behavioral decoding.
//!
//! This crate defines the Radxa Linkr Debugger logic-decoder JSON schema and
//! decodes packed digital samples into project-owned annotations. It is not a
//! libsigrokdecode Python plugin compatibility layer, and it does not copy GPL
//! decoder code or output templates.

pub mod annotation;
pub mod input;
pub mod protocols;
pub mod schema;

#[cfg(feature = "wasm")]
pub mod wasm;

use serde_json::json;

pub use annotation::{Annotation, Diagnostic, DiagnosticSeverity};
pub use input::{ChannelMapping, PackedSamples};
pub use protocols::{BitOrder, I2cOptions, Parity, ProtocolRequest, SpiOptions, UartOptions};
pub use schema::{DecodeError, DecodeRequest, DecodeResult, RESULT_SCHEMA_VERSION, SCHEMA_VERSION};

pub fn decode(request: &DecodeRequest) -> Result<DecodeResult, DecodeError> {
    if request.schema_version != SCHEMA_VERSION {
        return Err(DecodeError::UnsupportedSchema(
            request.schema_version.clone(),
        ));
    }
    input::validate_samples(&request.samples)?;

    let mut result = DecodeResult::new();

    match &request.protocol {
        ProtocolRequest::Uart(options) => {
            protocols::uart::decode(&request.samples, options, &mut result)?
        }
        ProtocolRequest::I2c(options) => {
            protocols::i2c::decode(&request.samples, options, &mut result)?
        }
        ProtocolRequest::Spi(options) => {
            protocols::spi::decode(&request.samples, options, &mut result)?
        }
    }

    Ok(result)
}

pub fn decode_json(request_json: &str) -> String {
    let response = match serde_json::from_str::<DecodeRequest>(request_json) {
        Ok(request) => match decode(&request) {
            Ok(result) => json!({ "ok": true, "result": result }),
            Err(error) => json!({ "ok": false, "error": error.to_string() }),
        },
        Err(error) => json!({ "ok": false, "error": format!("invalid JSON request: {error}") }),
    };
    response.to_string()
}
