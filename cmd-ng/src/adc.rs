// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::json_contract::{parse_envelope, EnvelopeError, JsonError};
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct AdcSensorValue {
    pub val1: i32,
    pub val2: i32,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct AdcReading {
    pub name: String,
    #[serde(default)]
    pub signal: String,
    pub raw: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub current_valid: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub mv: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub ma_est: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub power_enabled: Option<bool>,
    #[serde(default)]
    pub sensor_channel: String,
    #[serde(default)]
    pub unit: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub sensor_value: Option<AdcSensorValue>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub current_ua: Option<i32>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct AdcResponse {
    pub schema: String,
    pub ok: bool,
    pub command: String,
    #[serde(default)]
    pub action: String,
    #[serde(default)]
    pub readings: Vec<AdcReading>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<JsonError>,
}

pub fn transform_response(output: &str) -> Result<AdcResponse, String> {
    let response: AdcResponse = serde_json::from_str(output).map_err(|err| err.to_string())?;
    let readings = transform_readings(response.readings)?;
    Ok(AdcResponse {
        readings,
        ..response
    })
}

pub fn transform_readings(readings: Vec<AdcReading>) -> Result<Vec<AdcReading>, String> {
    readings
        .into_iter()
        .map(transform_reading)
        .collect::<Result<Vec<_>, _>>()
}

pub fn write_json(output: &str, command: &str) -> Result<String, EnvelopeError> {
    let envelope = parse_envelope(output, true)?;
    if envelope.ok == Some(false) {
        return Ok(output.trim().to_string());
    }
    transform_response(output)
        .map(|response| serde_json::to_string(&response).expect("serialize adc response"))
        .map_err(|err| EnvelopeError::Decode(format!("{command}: {err}")))
}

pub fn write_text(output: &str, verbose: bool) -> Result<String, String> {
    let envelope = parse_envelope(output, true).map_err(|err| err.to_string())?;
    if envelope.ok == Some(false) {
        return Err(envelope
            .error
            .map(|err| format!("{}: {}", err.code, err.message))
            .unwrap_or_else(|| "board returned adc error".to_string()));
    }

    let response = transform_response(output)?;
    let mut lines = Vec::new();
    for reading in response.readings {
        if verbose {
            let mut parts = vec![reading.name.clone()];
            if !reading.signal.is_empty() {
                parts.push(format!("signal={}", reading.signal));
            }
            if let Some(power_enabled) = reading.power_enabled {
                parts.push(format!(
                    "power={}",
                    if power_enabled { "on" } else { "off" }
                ));
            }
            if let Some(current_ua) = reading.current_ua {
                parts.push(format!(
                    "current={}",
                    current_text_from_microamps(current_ua)
                ));
                parts.push(format!("current_ua={current_ua}"));
            }
            match reading.raw {
                Some(raw) => parts.push(format!("raw={raw}")),
                None => parts.push("raw=null".to_string()),
            }
            if let Some(mv) = reading.mv {
                parts.push(format!("mv={mv}"));
            }
            if let Some(ma_est) = reading.ma_est {
                parts.push(format!("ma_est={ma_est}"));
            }
            lines.push(parts.join(" "));
            continue;
        }

        if let Some(current_ua) = reading.current_ua {
            lines.push(format!(
                "{}={}",
                reading.name,
                current_text_from_microamps(current_ua)
            ));
        } else if let Some(ma_est) = reading.ma_est {
            lines.push(format!("{}={}mA", reading.name, ma_est));
        }
    }

    Ok(lines.join("\n"))
}

fn transform_reading(mut reading: AdcReading) -> Result<AdcReading, String> {
    let current_ua = match (reading.current_ua, reading.sensor_value.clone()) {
        (Some(current_ua), _) => current_ua,
        (None, Some(sensor_value)) => sensor_value_to_microamps(sensor_value),
        (None, None) => return Ok(reading),
    };

    reading.current_ua = Some(current_ua);
    if reading.sensor_value.is_none() {
        reading.sensor_value = Some(sensor_value_from_microamps(current_ua));
    }
    if reading.sensor_channel.is_empty() {
        reading.sensor_channel = "current".to_string();
    }
    if reading.unit.is_empty() {
        reading.unit = "A".to_string();
    }
    Ok(reading)
}

fn sensor_value_to_microamps(value: AdcSensorValue) -> i32 {
    value.val1 * 1_000_000 + value.val2
}

fn sensor_value_from_microamps(current_ua: i32) -> AdcSensorValue {
    AdcSensorValue {
        val1: current_ua / 1_000_000,
        val2: current_ua % 1_000_000,
    }
}

fn current_text_from_microamps(current_ua: i32) -> String {
    let (prefix, abs_ua) = if current_ua < 0 {
        ("-", -current_ua)
    } else {
        ("", current_ua)
    };
    format!(
        "{}{abs}.{frac:06}A",
        prefix,
        abs = abs_ua / 1_000_000,
        frac = abs_ua % 1_000_000
    )
}

#[cfg(test)]
mod tests {
    use super::{transform_response, AdcResponse};

    #[test]
    fn derives_current_from_sensor_value_without_calibration() {
        let raw = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","readings":[{"name":"12v_out","signal":"S_C_12V","sensor_value":{"val1":0,"val2":1200000}}]}"#;
        let transformed = transform_response(raw).unwrap();
        let reading = transformed.readings.first().unwrap();
        assert_eq!(reading.current_ua, Some(1_200_000));
        assert_eq!(reading.ma_est, None);
        assert_eq!(reading.mv, None);
    }

    #[test]
    fn preserves_existing_ma_est() {
        let raw = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","readings":[{"name":"12v_out","ma_est":1}]}"#;
        let transformed: AdcResponse = transform_response(raw).unwrap();
        assert_eq!(transformed.readings[0].ma_est, Some(1));
    }

    #[test]
    fn leaves_unknown_current_shape_unchanged() {
        let raw = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","readings":[{"name":"12v_out"}]}"#;
        let transformed: AdcResponse = transform_response(raw).unwrap();
        assert_eq!(transformed.readings[0].current_ua, None);
    }
}
