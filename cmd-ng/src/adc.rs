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
    pub current_valid: Option<bool>,
    pub mv: Option<i32>,
    pub ma_est: Option<i32>,
    pub power_enabled: Option<bool>,
    #[serde(default)]
    pub sensor_channel: String,
    #[serde(default)]
    pub unit: String,
    pub sensor_value: Option<AdcSensorValue>,
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

#[derive(Clone, Copy)]
struct OffsetPoint {
    nominal_ma: i32,
    offset_ma: i32,
}

struct ChannelModel {
    name: &'static str,
    signal: &'static str,
    ma_per_mv: i32,
    offset_points: &'static [OffsetPoint],
}

const FIVE_VOLT_OFFSET_POINTS: &[OffsetPoint] = &[
    OffsetPoint {
        nominal_ma: 550,
        offset_ma: -550,
    },
    OffsetPoint {
        nominal_ma: 600,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 650,
        offset_ma: -350,
    },
    OffsetPoint {
        nominal_ma: 800,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 850,
        offset_ma: -350,
    },
    OffsetPoint {
        nominal_ma: 1000,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 1050,
        offset_ma: -350,
    },
    OffsetPoint {
        nominal_ma: 1200,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 1300,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 1350,
        offset_ma: -350,
    },
    OffsetPoint {
        nominal_ma: 1450,
        offset_ma: -350,
    },
    OffsetPoint {
        nominal_ma: 1650,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 1800,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 1900,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 2000,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 2050,
        offset_ma: -350,
    },
    OffsetPoint {
        nominal_ma: 2150,
        offset_ma: -350,
    },
    OffsetPoint {
        nominal_ma: 2300,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 2400,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 2450,
        offset_ma: -350,
    },
    OffsetPoint {
        nominal_ma: 2600,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 2650,
        offset_ma: -350,
    },
    OffsetPoint {
        nominal_ma: 2800,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 2900,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 3050,
        offset_ma: -450,
    },
    OffsetPoint {
        nominal_ma: 3100,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 3250,
        offset_ma: -450,
    },
    OffsetPoint {
        nominal_ma: 3300,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 3400,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 3500,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 3700,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 3850,
        offset_ma: -450,
    },
    OffsetPoint {
        nominal_ma: 3900,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 4050,
        offset_ma: -450,
    },
    OffsetPoint {
        nominal_ma: 4100,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 4300,
        offset_ma: -500,
    },
    OffsetPoint {
        nominal_ma: 4350,
        offset_ma: -450,
    },
    OffsetPoint {
        nominal_ma: 4450,
        offset_ma: -450,
    },
    OffsetPoint {
        nominal_ma: 4550,
        offset_ma: -450,
    },
    OffsetPoint {
        nominal_ma: 4600,
        offset_ma: -400,
    },
    OffsetPoint {
        nominal_ma: 4750,
        offset_ma: -450,
    },
];

const CHANNEL_MODELS: &[ChannelModel] = &[
    ChannelModel {
        name: "5v_out",
        signal: "S_C_5V",
        ma_per_mv: 50,
        offset_points: FIVE_VOLT_OFFSET_POINTS,
    },
    ChannelModel {
        name: "12v_out",
        signal: "S_C_12V",
        ma_per_mv: 50,
        offset_points: &[],
    },
    ChannelModel {
        name: "20v_out",
        signal: "S_C_20V",
        ma_per_mv: 50,
        offset_points: &[],
    },
];

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
            parts.push("raw=null".to_string());
            if let Some(mv) = reading.mv {
                parts.push(format!("mv={mv}"));
            }
            if let Some(ma_est) = reading.ma_est {
                parts.push(format!("ma_est={ma_est}"));
            }
            lines.push(parts.join(" "));
            continue;
        }

        if let Some(ma_est) = reading.ma_est {
            lines.push(format!("{}={}mA", reading.name, ma_est));
        } else if let Some(current_ua) = reading.current_ua {
            lines.push(format!(
                "{}={}",
                reading.name,
                current_text_from_microamps(current_ua)
            ));
        }
    }

    Ok(lines.join("\n"))
}

fn transform_reading(mut reading: AdcReading) -> Result<AdcReading, String> {
    if reading.ma_est.is_some() {
        return Ok(reading);
    }

    let model = find_channel_model(&reading)
        .ok_or_else(|| format!("unknown ADC channel {:?}", reading.name))?;
    let current_ua = match (reading.current_ua, reading.sensor_value.clone()) {
        (Some(current_ua), _) => current_ua,
        (None, Some(sensor_value)) => sensor_value_to_microamps(sensor_value),
        (None, None) => return Err(format!("reading {:?} missing current value", reading.name)),
    };

    let nominal_ma = current_ua / 1000;
    let mv = nominal_mv_from_current_ua(current_ua, model);
    let mut ma_est = apply_current_offset(nominal_ma, model);
    if reading.power_enabled == Some(false) {
        ma_est = 0;
    }

    reading.raw = None;
    reading.current_valid = Some(true);
    reading.mv = Some(mv);
    reading.ma_est = Some(ma_est);
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

fn find_channel_model(reading: &AdcReading) -> Option<&'static ChannelModel> {
    CHANNEL_MODELS
        .iter()
        .find(|model| model.name == reading.name || model.signal == reading.signal)
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

fn nominal_mv_from_current_ua(current_ua: i32, model: &ChannelModel) -> i32 {
    let nominal_ma = current_ua / 1000;
    if nominal_ma <= 0 || model.ma_per_mv == 0 {
        return 0;
    }
    nominal_ma / model.ma_per_mv
}

fn apply_current_offset(nominal_ma: i32, model: &ChannelModel) -> i32 {
    let points = model.offset_points;
    if let Some(first) = points.first() {
        if nominal_ma <= first.nominal_ma {
            return (nominal_ma + first.offset_ma).max(0);
        }

        for window in points.windows(2) {
            let prev = window[0];
            let next = window[1];
            if nominal_ma > next.nominal_ma {
                continue;
            }
            let den = next.nominal_ma - prev.nominal_ma;
            if den <= 0 {
                return (nominal_ma + next.offset_ma).max(0);
            }
            let num = i64::from(nominal_ma - prev.nominal_ma)
                * i64::from(next.offset_ma - prev.offset_ma);
            return (nominal_ma
                + prev.offset_ma
                + ((num + i64::from(den / 2)) / i64::from(den)) as i32)
                .max(0);
        }

        if points.len() >= 2 {
            let prev = points[points.len() - 2];
            let next = points[points.len() - 1];
            let den = next.nominal_ma - prev.nominal_ma;
            if den <= 0 {
                return (nominal_ma + next.offset_ma).max(0);
            }
            let num = i64::from(nominal_ma - next.nominal_ma)
                * i64::from(next.offset_ma - prev.offset_ma);
            return (nominal_ma
                + next.offset_ma
                + ((num + i64::from(den / 2)) / i64::from(den)) as i32)
                .max(0);
        }

        return (nominal_ma + points[points.len() - 1].offset_ma).max(0);
    }

    nominal_ma.max(0)
}

#[cfg(test)]
mod tests {
    use super::{transform_response, AdcResponse};

    #[test]
    fn enriches_adc_reading() {
        let raw = r#"{"schema":"agent-debugboard.v1","ok":true,"command":"adc","readings":[{"name":"12v_out","signal":"S_C_12V","sensor_value":{"val1":0,"val2":1200000}}]}"#;
        let transformed = transform_response(raw).unwrap();
        let reading = transformed.readings.first().unwrap();
        assert_eq!(reading.ma_est, Some(1200));
        assert_eq!(reading.mv, Some(24));
    }

    #[test]
    fn preserves_existing_ma_est() {
        let raw = r#"{"schema":"agent-debugboard.v1","ok":true,"command":"adc","readings":[{"name":"12v_out","ma_est":1}]}"#;
        let transformed: AdcResponse = transform_response(raw).unwrap();
        assert_eq!(transformed.readings[0].ma_est, Some(1));
    }
}
