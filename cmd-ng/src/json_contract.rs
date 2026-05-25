use serde::{Deserialize, Serialize};
use serde_json::Value;

pub const JSON_SCHEMA: &str = "radxa-linkr-debugger.v1";

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct JsonError {
    pub code: String,
    pub message: String,
}

#[derive(Debug, Clone, Deserialize)]
pub struct JsonEnvelope {
    pub ok: Option<bool>,
    pub error: Option<JsonError>,
}

#[derive(Debug, Clone, Serialize)]
pub struct JsonFailure<'a> {
    pub schema: &'a str,
    pub ok: bool,
    pub command: &'a str,
    pub error: JsonError,
}

#[derive(Debug, thiserror::Error)]
pub enum EnvelopeError {
    #[error("firmware returned empty output")]
    Empty,
    #[error("firmware returned non-JSON output")]
    InvalidJson,
    #[error("firmware returned JSON without radxa-linkr-debugger.v1 envelope")]
    MissingEnvelope,
    #[error("{0}")]
    Decode(String),
}

pub fn parse_envelope(output: &str, require_envelope: bool) -> Result<JsonEnvelope, EnvelopeError> {
    let cleaned = output.trim();
    if cleaned.is_empty() {
        return Err(EnvelopeError::Empty);
    }
    let value: Value = serde_json::from_str(cleaned).map_err(|_| EnvelopeError::InvalidJson)?;
    if !require_envelope {
        return Ok(JsonEnvelope {
            ok: None,
            error: None,
        });
    }

    let schema = value.get("schema").and_then(Value::as_str);
    let ok = value.get("ok");
    let command = value.get("command").and_then(Value::as_str);
    if schema != Some(JSON_SCHEMA) || ok.is_none() || command.is_none() || command == Some("") {
        return Err(EnvelopeError::MissingEnvelope);
    }

    let envelope: JsonEnvelope =
        serde_json::from_str(cleaned).map_err(|err| EnvelopeError::Decode(err.to_string()))?;
    Ok(envelope)
}

pub fn render_failure(command: &str, code: &str, message: &str) -> String {
    serde_json::to_string(&JsonFailure {
        schema: JSON_SCHEMA,
        ok: false,
        command,
        error: JsonError {
            code: code.to_string(),
            message: message.to_string(),
        },
    })
    .expect("serialize JsonFailure")
}

#[cfg(test)]
mod tests {
    use super::{parse_envelope, EnvelopeError};

    #[test]
    fn rejects_missing_envelope() {
        let err = parse_envelope("{\"hello\":\"world\"}", true).unwrap_err();
        assert!(matches!(err, EnvelopeError::MissingEnvelope));
    }
}
