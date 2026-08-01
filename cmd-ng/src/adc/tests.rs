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
