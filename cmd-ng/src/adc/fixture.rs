use serde_json::{json, Value};

pub(crate) fn channels() -> Value {
    json!([
        {"name":"5v_out","signal":"S_C_5V","kind":"current","unit":"uA","sensor_channel":"current"},
        {"name":"12v_out","signal":"S_C_12V","kind":"current","unit":"uA","sensor_channel":"current"},
        {"name":"20v_out","signal":"S_C_20V","kind":"current","unit":"uA","sensor_channel":"current"},
        {"name":"vdd_5v","signal":"S_C_VDD_5V","kind":"current","unit":"uA","sensor_channel":"current"},
        {"name":"adc0","signal":"GPIO26_ADC0","kind":"voltage","role":"pd_input","unit":"uV","sensor_channel":"voltage","note":"source voltage"},
        {"name":"adc1","signal":"GPIO27_ADC1","kind":"voltage","role":"native","unit":"uV","sensor_channel":"voltage"},
        {"name":"adc2","signal":"GPIO28_ADC2","kind":"voltage","role":"native","unit":"uV","sensor_channel":"voltage"},
        {"name":"adc3","signal":"GPIO29_ADC3","kind":"voltage","role":"native","unit":"uV","sensor_channel":"voltage"}
    ])
}

pub(crate) fn batch() -> Value {
    json!({
        "type":"telemetry-batch", "topic":"adc", "schema":crate::json_contract::JSON_SCHEMA,
        "channels":channels(),
        "samples":[{
            "sequence":14, "uptime_us":4321, "power_enabled_mask":15,
            "voltage_valid_mask":13, "voltages_uv":[5_000_000, 0, 20_000_000, 5_100_000],
            "values":[100, 200, 300, 400, 20_000_002, 1_100_000, 2_200_000, 0]
        }]
    })
}

pub(crate) fn http() -> Value {
    let batch = batch();
    let readings: Vec<_> = channels()
        .as_array()
        .unwrap()
        .iter()
        .enumerate()
        .map(|(index, channel)| {
            let mut reading = channel.clone();
            reading["value"] = batch["samples"][0]["values"][index].clone();
            if index == 4 {
                reading["raw"] = json!(3237);
                reading["mv"] = json!(2608);
            }
            reading
        })
        .collect();
    json!({"schema":crate::json_contract::JSON_SCHEMA,"ok":true,"command":"adc","action":"read","readings":readings})
}
