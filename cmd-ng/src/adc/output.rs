use crate::json_contract::{parse_envelope, EnvelopeError};

use super::{transform_response, AdcKind};

pub fn write_json(output: &str, command: &str) -> Result<String, EnvelopeError> {
    let envelope = parse_envelope(output, true)?;
    if envelope.ok == Some(false) {
        return Ok(output.trim().to_string());
    }
    transform_response(output)
        .map_err(|err| EnvelopeError::Decode(format!("{command}: {err}")))
        .and_then(|response| {
            serde_json::to_string(&response)
                .map_err(|err| EnvelopeError::Decode(format!("{command}: {err}")))
        })
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
            if let Some(voltage_uv) = reading.voltage_uv {
                parts.push(format!(
                    "voltage={}",
                    voltage_text_from_microvolts(voltage_uv)
                ));
                parts.push(format!("voltage_uv={voltage_uv}"));
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
            match reading.kind {
                AdcKind::Current => lines.push(format!(
                    "{}={}",
                    reading.name,
                    current_text_from_microamps(current_ua)
                )),
                AdcKind::Voltage => {}
            }
        } else {
            match reading.kind {
                AdcKind::Current => {
                    if let Some(ma_est) = reading.ma_est {
                        lines.push(format!("{}={}mA", reading.name, ma_est));
                    }
                }
                AdcKind::Voltage => {
                    if let Some(voltage_uv) = reading.voltage_uv {
                        lines.push(format!(
                            "{}={}",
                            reading.name,
                            voltage_text_from_microvolts(voltage_uv)
                        ));
                    }
                }
            }
        }
    }
    Ok(lines.join("\n"))
}

fn current_text_from_microamps(current_ua: i32) -> String {
    let (prefix, abs_ua) = if current_ua < 0 {
        ("-", -(current_ua as i64))
    } else {
        ("", current_ua as i64)
    };
    format!(
        "{}{abs}.{frac:06}A",
        prefix,
        abs = abs_ua / 1_000_000,
        frac = abs_ua % 1_000_000
    )
}

fn voltage_text_from_microvolts(voltage_uv: i32) -> String {
    format!(
        "{}{:.6}V",
        if voltage_uv < 0 { "-" } else { "" },
        (voltage_uv as f64).abs() / 1_000_000.0
    )
}
