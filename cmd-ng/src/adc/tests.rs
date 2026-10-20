use super::{transform_response, AdcKind, AdcResponse};

#[test]
fn parses_strict_compact_adc_kind_and_value_fields() {
    let raw = r#"{"name":"adc3","signal":"ADC3","kind":"voltage","unit":"uV","value":1234000}"#;
    let reading: super::AdcReading = serde_json::from_str(raw).unwrap();

    assert_eq!(reading.kind, AdcKind::Voltage);
    assert_eq!(reading.unit, "uV");
    assert_eq!(reading.value, Some(1_234_000));
}

#[test]
fn rejects_unknown_compact_adc_kind() {
    let raw = r#"{"name":"adc3","kind":"resistance","unit":"uV","value":1}"#;

    assert!(serde_json::from_str::<super::AdcReading>(raw).is_err());
}

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

#[test]
fn normalizes_http_voltage_without_fabricating_current() {
    let raw = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","readings":[{"name":"adc3","signal":"ADC3","raw":42,"mv":1234,"sensor_channel":"voltage","unit":"V","sensor_value":{"val1":1,"val2":234000},"current_ua":99}]}"#;
    let transformed = transform_response(raw).unwrap();
    let reading = &transformed.readings[0];

    assert_eq!(reading.kind, AdcKind::Voltage);
    assert_eq!(reading.voltage_uv, Some(1_234_000));
    assert_eq!(reading.value, Some(1_234_000));
    assert_eq!(reading.current_ua, None);
}

#[test]
fn renders_http_voltage_as_volts() {
    let raw = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","readings":[{"name":"adc3","sensor_channel":"voltage","unit":"V","sensor_value":{"val1":1,"val2":234000}}]}"#;

    assert_eq!(super::write_text(raw, false).unwrap(), "adc3=1.234000V");
}

#[test]
fn invalid_http_rail_voltage_stays_unknown_in_normalized_and_json_output() {
    for voltage_uv in [serde_json::Value::Null, serde_json::json!(5_000_000)] {
        for current_ua in [serde_json::Value::Null, serde_json::json!(123_000)] {
            let raw = serde_json::json!({
                "schema": "radxa-linkr-debugger.v1", "ok": true, "command": "adc",
                "readings": [{
                    "name": "vdd_5v", "sensor_channel": "current", "unit": "A",
                    "current_ua": current_ua, "voltage_valid": false,
                    "voltage_uv": voltage_uv, "mv": 0
                }]
            })
            .to_string();
            let response = transform_response(&raw).unwrap();
            assert_eq!(response.readings[0].voltage_valid, Some(false));
            assert_eq!(response.readings[0].voltage_uv, None);

            let json: serde_json::Value =
                serde_json::from_str(&super::write_json(&raw, "adc").unwrap()).unwrap();
            assert_eq!(json["readings"][0]["voltage_valid"], false);
            assert!(json["readings"][0]["voltage_uv"].is_null());
            assert!(!super::write_text(&raw, false).unwrap().contains("voltage="));
        }
    }
}

#[test]
fn valid_http_rail_voltage_preserves_measured_zero_and_mv_conversion() {
    for (voltage_uv, mv, expected) in [
        (Some(0), 5000, 0),
        (Some(5_100_000), 5000, 5_100_000),
        (None, 5000, 5_000_000),
    ] {
        let raw = serde_json::json!({
            "schema": "radxa-linkr-debugger.v1", "ok": true, "command": "adc",
            "readings": [{
                "name": "vdd_5v", "current_ua": 123_000, "voltage_valid": true,
                "voltage_uv": voltage_uv, "mv": mv
            }]
        })
        .to_string();
        let response = transform_response(&raw).unwrap();
        assert_eq!(response.readings[0].voltage_valid, Some(true));
        assert_eq!(response.readings[0].voltage_uv, Some(expected));
    }
}

#[test]
fn compact_rail_voltage_validity_survives_normalization() {
    for (valid, expected) in [(false, None), (true, Some(0))] {
        let compact: super::AdcCompactReading = serde_json::from_value(serde_json::json!({
            "name": "vdd_5v", "kind": "current", "unit": "uA", "value": 123_000,
            "voltage_valid": valid, "voltage_uv": 0
        }))
        .unwrap();
        let readings = super::transform_readings(vec![compact.into_reading().unwrap()]).unwrap();
        assert_eq!(readings[0].voltage_valid, Some(valid));
        assert_eq!(readings[0].voltage_uv, expected);
    }
}
