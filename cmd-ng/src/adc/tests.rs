use super::{transform_response, AdcKind, AdcResponse, AdcRole};

#[test]
fn all_eight_http_channels_keep_canonical_microvolts_roles_and_generic_metadata() {
    let raw = super::fixture::http().to_string();
    let response = transform_response(&raw).unwrap();
    assert_eq!(
        response
            .readings
            .iter()
            .map(|reading| reading.name.as_str())
            .collect::<Vec<_>>(),
        ["5v_out", "12v_out", "20v_out", "vdd_5v", "adc0", "adc1", "adc2", "adc3"]
    );
    for (reading, value) in response.readings[4..]
        .iter()
        .zip([20_000_002, 1_100_000, 2_200_000, 0])
    {
        assert_eq!(reading.kind, AdcKind::Voltage);
        assert_eq!(reading.voltage_uv, Some(value));
        assert_eq!(reading.current_ua, None);
        assert_eq!(reading.power_enabled, None);
    }
    assert_eq!(response.readings[4].role, Some(AdcRole::PdInput));
    assert_eq!(response.readings[4].mv, Some(2608));
    assert_eq!(response.readings[4].raw, Some(3237));
    assert!(response.readings[5..]
        .iter()
        .all(|reading| reading.role == Some(AdcRole::Native)));
    assert_eq!(response.readings[4].note, "source voltage");
    assert!(super::write_text(&raw, false)
        .unwrap()
        .contains("adc0=20.000002V source voltage"));
    let mut rich = super::fixture::http();
    for (index, value) in [20_000_002, 1_100_000, 2_200_000, 0]
        .into_iter()
        .enumerate()
    {
        let reading = &mut rich["readings"][index + 4];
        reading.as_object_mut().unwrap().remove("value");
        reading["unit"] = serde_json::json!("V");
        reading["sensor_value"] =
            serde_json::json!({"val1":value / 1_000_000,"val2":value % 1_000_000});
        reading["label"] = serde_json::json!(format!("采样{index}"));
    }
    let rich = transform_response(&rich.to_string()).unwrap();
    assert_eq!(rich.readings[4].voltage_uv, Some(20_000_002));
    assert_eq!(rich.readings[4].mv, Some(2608));
    assert_eq!(rich.readings[7].voltage_uv, Some(0));
    assert_eq!(rich.readings[4].label, "采样0");
}

#[test]
fn json_and_verbose_output_distinguish_source_voltage_from_pad_diagnostics() {
    let raw = super::fixture::http().to_string();
    let json: serde_json::Value =
        serde_json::from_str(&super::write_json(&raw, "adc").unwrap()).unwrap();
    let adc0 = &json["readings"][4];
    assert_eq!(adc0["role"], "pd_input");
    assert_eq!(adc0["voltage_uv"], 20_000_002);
    assert_eq!(adc0["raw"], 3237);
    assert_eq!(adc0["mv"], 2608);
    assert_eq!(json["readings"][5]["role"], "native");

    let text = super::write_text(&raw, true).unwrap();
    let adc0 = text.lines().find(|line| line.starts_with("adc0 ")).unwrap();
    for field in [
        "role=pd_input",
        "source_voltage=20.000002V",
        "source_voltage_uv=20000002",
        "raw=3237",
        "pad_mv=2608",
    ] {
        assert!(adc0.contains(field), "{adc0}");
    }
    assert!(text.contains("adc1 signal=GPIO27_ADC1 role=native voltage=1.100000V"));
}

#[test]
fn optional_roles_never_infer_or_rescale_voltage_from_channel_names() {
    for role in [None, Some(AdcRole::Native), Some(AdcRole::PdInput)] {
        let mut wire = super::fixture::http();
        for reading in wire["readings"].as_array_mut().unwrap() {
            reading.as_object_mut().unwrap().remove("role");
        }
        if let Some(role) = role {
            wire["readings"][4]["role"] = serde_json::to_value(role).unwrap();
        }
        let raw = wire.to_string();
        let readings = transform_response(&raw).unwrap().readings;
        let readings = super::transform_readings(readings).unwrap();
        assert_eq!(readings[4].role, role);
        assert_eq!(readings[4].voltage_uv, Some(20_000_002));
        assert_eq!(readings[4].mv, Some(2608));
        assert!(readings[5..].iter().all(|reading| reading.role.is_none()));
        let output: serde_json::Value =
            serde_json::from_str(&super::write_json(&raw, "adc").unwrap()).unwrap();
        assert_eq!(output["readings"][4].get("role").is_some(), role.is_some());
    }
}

#[test]
fn rejects_unknown_roles_in_http_and_compact_readings() {
    for role in [
        serde_json::json!("pad"),
        serde_json::json!(""),
        serde_json::json!(1),
    ] {
        let raw = serde_json::json!({
            "name":"adc0", "kind":"voltage", "unit":"uV", "role":role, "value":20_000_002
        });
        assert!(serde_json::from_value::<super::AdcReading>(raw.clone()).is_err());
        assert!(serde_json::from_value::<super::AdcCompactReading>(raw).is_err());
    }
}

#[test]
fn missing_and_invalid_canonical_values_stay_unknown_even_with_pad_diagnostics() {
    for extra in [
        serde_json::json!({}),
        serde_json::json!({"voltage_valid":false,"value":123}),
    ] {
        let mut reading = serde_json::json!({
            "name":"arbitrary", "kind":"voltage", "role":"pd_input", "unit":"uV",
            "raw":3237, "mv":2608
        });
        reading
            .as_object_mut()
            .unwrap()
            .extend(extra.as_object().unwrap().clone());
        let readings =
            super::transform_readings(vec![serde_json::from_value(reading).unwrap()]).unwrap();
        assert_eq!(readings[0].voltage_uv, None);
        assert_eq!(readings[0].value, None);
        assert_eq!(readings[0].mv, Some(2608));
        assert_eq!(readings[0].raw, Some(3237));
    }
}

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
