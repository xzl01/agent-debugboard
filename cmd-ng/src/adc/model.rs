use crate::json_contract::JsonError;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Deserialize, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum AdcKind {
    #[default]
    Current,
    Voltage,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct AdcSensorValue {
    pub val1: i32,
    pub val2: i32,
}

#[derive(Debug, Clone, Default, Deserialize, Serialize)]
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
    #[serde(default, skip_serializing)]
    pub kind: AdcKind,
    #[serde(default)]
    pub sensor_channel: String,
    #[serde(default)]
    pub unit: String,
    #[serde(skip_serializing)]
    pub value: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub sensor_value: Option<AdcSensorValue>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub current_ua: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub voltage_uv: Option<i32>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct AdcCompactReading {
    pub name: String,
    #[serde(default)]
    pub signal: String,
    pub kind: AdcKind,
    pub unit: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub power_enabled: Option<bool>,
    pub value: i32,
}

impl AdcCompactReading {
    pub fn into_reading(self) -> Result<AdcReading, String> {
        validate_compact_kind_unit(self.kind, &self.unit)?;
        let (current_ua, voltage_uv, sensor_channel) = match self.kind {
            AdcKind::Current => (Some(self.value), None, "current"),
            AdcKind::Voltage => (None, Some(self.value), "voltage"),
        };
        Ok(AdcReading {
            name: self.name,
            signal: self.signal,
            raw: None,
            current_valid: None,
            mv: None,
            ma_est: None,
            power_enabled: self.power_enabled,
            kind: self.kind,
            sensor_channel: sensor_channel.to_string(),
            unit: self.unit,
            value: Some(self.value),
            sensor_value: None,
            current_ua,
            voltage_uv,
        })
    }
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

pub fn validate_compact_kind_unit(kind: AdcKind, unit: &str) -> Result<(), String> {
    let valid = match kind {
        AdcKind::Current => unit == "uA",
        AdcKind::Voltage => unit == "uV",
    };
    if valid {
        Ok(())
    } else {
        Err(format!(
            "ADC kind {:?} does not match unit {:?}",
            kind, unit
        ))
    }
}
