use super::{AdcKind, AdcReading, AdcResponse, AdcSensorValue};

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

fn transform_reading(mut reading: AdcReading) -> Result<AdcReading, String> {
    reading.kind = match reading.sensor_channel.as_str() {
        "current" => AdcKind::Current,
        "voltage" => AdcKind::Voltage,
        _ => reading.kind,
    };
    if reading.sensor_channel.is_empty() {
        reading.sensor_channel = match reading.kind {
            AdcKind::Current => "current".to_string(),
            AdcKind::Voltage => "voltage".to_string(),
        };
    }
    if reading.unit.is_empty() {
        reading.unit = match reading.kind {
            AdcKind::Current => "A".to_string(),
            AdcKind::Voltage => "V".to_string(),
        };
    }

    match reading.kind {
        AdcKind::Current => {
            if reading.unit != "A" && reading.unit != "uA" {
                return Err(format!("invalid current ADC unit {:?}", reading.unit));
            }
            let current_ua = match (
                reading.current_ua,
                reading.value,
                reading.sensor_value.clone(),
            ) {
                (Some(current_ua), _, _) => current_ua,
                (None, Some(value), _) => value,
                (None, None, Some(sensor_value)) => sensor_value_to_microamps(sensor_value),
                (None, None, None) => return Ok(reading),
            };
            reading.current_ua = Some(current_ua);
            reading.value = Some(current_ua);
            if reading.sensor_value.is_none() {
                reading.sensor_value = Some(sensor_value_from_microamps(current_ua));
            }
        }
        AdcKind::Voltage => {
            if reading.unit != "V" && reading.unit != "uV" {
                return Err(format!("invalid voltage ADC unit {:?}", reading.unit));
            }
            reading.current_valid = None;
            reading.ma_est = None;
            reading.power_enabled = None;
            reading.current_ua = None;
            let voltage_uv = match (
                reading.voltage_uv,
                reading.value,
                reading.sensor_value.clone(),
            ) {
                (Some(voltage_uv), _, _) => voltage_uv,
                (None, Some(value), _) => value,
                (None, None, Some(sensor_value)) => sensor_value_to_microvolts(sensor_value),
                (None, None, None) => return Ok(reading),
            };
            reading.value = Some(voltage_uv);
            reading.voltage_uv = Some(voltage_uv);
        }
    }
    Ok(reading)
}

fn sensor_value_to_microamps(value: AdcSensorValue) -> i32 {
    value.val1 * 1_000_000 + value.val2
}

fn sensor_value_to_microvolts(value: AdcSensorValue) -> i32 {
    value.val1 * 1_000_000 + value.val2
}

fn sensor_value_from_microamps(current_ua: i32) -> AdcSensorValue {
    AdcSensorValue {
        val1: current_ua / 1_000_000,
        val2: current_ua % 1_000_000,
    }
}
