// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::adc::AdcKind;
use crate::client::{BoardClient, BoardRequest, BoardTransport};
use crate::json_contract::JSON_SCHEMA;
use crate::ws_client::{
    expand_telemetry_batch, subscribe_batch_request, subscribe_request, WsClient, WsTelemetryBatch,
    WsTelemetryMessage, MAX_ADC_BATCH_SAMPLES,
};
use anyhow::{anyhow, Result};
use reqwest::Method;
use serde::Deserialize;
use serde_json::json;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::Path;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

pub const MAX_ADC_RECORD_RATE_HZ: i32 = 250;
pub const DEFAULT_ADC_RECORD_RATE_HZ: i32 = MAX_ADC_RECORD_RATE_HZ;
const ADC_RECORD_BATCH_SIZE: u8 = MAX_ADC_BATCH_SAMPLES;

const DEVICE_TIMING_FIELDS: &[&str] = &[
    "device_t_mono_ns",
    "device_t_mono_us",
    "device_t_unix_ns",
    "device_timestamp_ns",
    "sample_sequence",
    "sample_t_mono_ns",
    "sample_t_unix_ns",
    "sample_timestamp_ns",
    "timestamp_ns",
    "uptime_ns",
    "uptime_us",
    "uptime_ms",
    "uptime_seconds",
];

const CSV_RAILS: &[&str] = &["5v_out", "12v_out", "20v_out", "vdd_5v"];

#[derive(Debug, Clone, Deserialize)]
struct LiveSessionCreateResponse {
    ok: bool,
    #[serde(default)]
    session_id: Option<u32>,
    #[serde(default)]
    ws_url: String,
}

struct LiveSession {
    id: u32,
    ws_url: String,
}

fn request_live_session(base_url: &str, timeout: Duration) -> Result<LiveSession> {
    let client = BoardClient::new(base_url, timeout)?;
    let data = client.send_text(BoardRequest {
        method: Method::POST,
        path: "/api/v1/live-sessions".to_string(),
        query: vec![],
        body: None,
    })?;
    let response: LiveSessionCreateResponse = serde_json::from_str(&data)?;
    if !response.ok || response.ws_url.trim().is_empty() {
        return Err(anyhow!("missing websocket URL in live session response"));
    }
    let session_id = response
        .session_id
        .ok_or_else(|| anyhow!("missing session_id in live session response"))?;
    Ok(LiveSession {
        id: session_id,
        ws_url: response.ws_url,
    })
}

fn delete_live_session(base_url: &str, timeout: Duration, session_id: u32) -> Result<()> {
    let client = BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::DELETE,
        path: format!("/api/v1/live-sessions/{session_id}"),
        query: vec![],
        body: None,
    })?;
    Ok(())
}

fn cleanup_live_session(
    base_url: &str,
    timeout: Duration,
    session_id: u32,
    ws_client: Option<&WsClient>,
) -> Result<()> {
    let delete_result = delete_live_session(base_url, timeout, session_id);
    let close_result = if let Some(ws_client) = ws_client {
        ws_client.close()
    } else {
        Ok(())
    };

    delete_result?;
    close_result?;
    Ok(())
}

fn finish_with_cleanup(primary: Result<()>, cleanup: Result<()>) -> Result<()> {
    match primary {
        Ok(()) => cleanup,
        Err(primary_err) => {
            if let Err(cleanup_err) = cleanup {
                eprintln!("live session cleanup failed after recorder error: {cleanup_err:#}");
            }
            Err(primary_err)
        }
    }
}

fn unix_time_ns() -> i128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos() as i128)
        .unwrap_or_default()
}

pub fn record_adc_ws_to_file(
    base_url: &str,
    timeout: Duration,
    output_path: &str,
    max_samples: Option<usize>,
    requested_rate_hz: i32,
) -> Result<()> {
    anyhow::ensure!(
        (1..=MAX_ADC_RECORD_RATE_HZ).contains(&requested_rate_hz),
        "ADC record rate must be in 1..={MAX_ADC_RECORD_RATE_HZ} Hz"
    );
    let session = request_live_session(base_url, timeout)?;
    let ws_client = WsClient::with_ws_url(base_url.to_string(), session.ws_url);
    let result =
        record_adc_ws_to_file_with_session(&ws_client, output_path, max_samples, requested_rate_hz);
    let cleanup = cleanup_live_session(base_url, timeout, session.id, Some(&ws_client));
    finish_with_cleanup(result, cleanup)
}

fn record_adc_ws_to_file_with_session(
    ws_client: &WsClient,
    output_path: &str,
    max_samples: Option<usize>,
    requested_rate_hz: i32,
) -> Result<()> {
    ws_client.connect()?;
    let subscribe = if requested_rate_hz > 100 {
        subscribe_batch_request(requested_rate_hz, ADC_RECORD_BATCH_SIZE)
    } else {
        subscribe_request(requested_rate_hz)
    };
    ws_client.send(&subscribe)?;

    let file = File::create(Path::new(output_path))?;
    let mut writer = BufWriter::with_capacity(1024 * 1024, file);
    let csv = Path::new(output_path)
        .extension()
        .and_then(|extension| extension.to_str())
        .is_some_and(|extension| extension.eq_ignore_ascii_case("csv"));
    let mut csv_voltage_channels: Option<Vec<String>> = None;
    let started = Instant::now();
    let mut written = 0usize;

    loop {
        if let Some(limit) = max_samples {
            if written >= limit {
                break;
            }
        }

        let text = ws_client.recv_text()?;
        let value: serde_json::Value = serde_json::from_str(&text)?;
        let mut messages = match value.get("type").and_then(|v| v.as_str()) {
            Some("telemetry") => vec![serde_json::from_value(value)?],
            Some("telemetry-batch") => {
                let batch: WsTelemetryBatch = serde_json::from_value(value)?;
                expand_telemetry_batch(batch)?
            }
            _ => continue,
        };

        for mut message in messages.drain(..) {
            if max_samples.is_some_and(|limit| written >= limit) {
                break;
            }
            message.readings =
                crate::adc::transform_readings(message.readings).map_err(|e| anyhow!(e))?;
            if csv {
                if csv_voltage_channels.is_none() {
                    let channels = message
                        .readings
                        .iter()
                        .filter(|reading| reading.kind == AdcKind::Voltage)
                        .map(|reading| reading.name.clone())
                        .collect::<Vec<_>>();
                    write_csv_header(&mut writer, &channels)?;
                    csv_voltage_channels = Some(channels);
                }
                write_telemetry_csv_row(
                    &mut writer,
                    &message,
                    started.elapsed(),
                    unix_time_ns(),
                    csv_voltage_channels.as_deref().unwrap_or_default(),
                )?;
            } else {
                write_telemetry_record(
                    &mut writer,
                    &message,
                    started.elapsed(),
                    unix_time_ns(),
                    requested_rate_hz,
                )?;
            }
            written += 1;
        }
    }

    if csv && csv_voltage_channels.is_none() {
        write_csv_header(&mut writer, &[])?;
    }
    writer.flush()?;
    Ok(())
}

fn write_csv_header(writer: &mut BufWriter<File>, voltage_channels: &[String]) -> Result<()> {
    write!(writer, "sequence,t_mono_ns,t_unix_ns,device_t_mono_us")?;
    for rail in CSV_RAILS {
        write!(
            writer,
            ",{rail}_power_enabled,{rail}_current_ua,{rail}_voltage_uv"
        )?;
    }
    for channel in voltage_channels {
        write!(writer, ",{channel}_voltage_uv")?;
    }
    writer.write_all(b"\n")?;
    Ok(())
}

fn csv_device_time_us(message: &WsTelemetryMessage) -> u64 {
    message
        .extra
        .get("device_t_mono_us")
        .and_then(|value| value.as_u64())
        .or_else(|| {
            message
                .extra
                .get("uptime_us")
                .and_then(|value| value.as_u64())
        })
        .unwrap_or_default()
}

fn write_telemetry_csv_row(
    writer: &mut BufWriter<File>,
    message: &WsTelemetryMessage,
    elapsed: Duration,
    unix_ns: i128,
    voltage_channels: &[String],
) -> Result<()> {
    let device_t_mono_us = csv_device_time_us(message);
    write!(
        writer,
        "{},{},{},{}",
        message.sequence.unwrap_or_default(),
        elapsed.as_nanos(),
        unix_ns,
        device_t_mono_us
    )?;
    for rail in CSV_RAILS {
        let reading = message
            .readings
            .iter()
            .find(|reading| reading.name == *rail && reading.kind == AdcKind::Current);
        write_csv_cell(writer, reading.and_then(|reading| reading.power_enabled))?;
        write_csv_cell(writer, reading.and_then(|reading| reading.current_ua))?;
        write_csv_cell(
            writer,
            reading
                .filter(|reading| reading.voltage_valid != Some(false))
                .and_then(|reading| reading.voltage_uv),
        )?;
    }
    for channel in voltage_channels {
        let reading = message
            .readings
            .iter()
            .find(|reading| reading.name == *channel && reading.kind == AdcKind::Voltage);
        write_csv_cell(
            writer,
            reading
                .filter(|reading| reading.voltage_valid != Some(false))
                .and_then(|reading| reading.voltage_uv),
        )?;
    }
    writer.write_all(b"\n")?;
    Ok(())
}

fn write_csv_cell(
    writer: &mut impl Write,
    value: Option<impl std::fmt::Display>,
) -> std::io::Result<()> {
    writer.write_all(b",")?;
    if let Some(value) = value {
        write!(writer, "{value}")?;
    }
    Ok(())
}

fn write_telemetry_record(
    writer: &mut BufWriter<File>,
    message: &WsTelemetryMessage,
    elapsed: Duration,
    unix_ns: i128,
    requested_rate_hz: i32,
) -> Result<()> {
    let mut metadata = json!({
        "requested_rate_hz": requested_rate_hz,
    });
    if let Some(device_timing) = device_timing_metadata(message) {
        metadata["device_timing"] = device_timing;
    }
    if let Some(dropped_samples) = message.extra.get("dropped_samples") {
        metadata["dropped_samples"] = dropped_samples.clone();
    }

    let record = json!({
        "schema": JSON_SCHEMA,
        "type": "telemetry-record",
        "metadata": metadata,
        "t_mono_ns": elapsed.as_nanos(),
        "t_unix_ns": unix_ns,
        "sequence": message.sequence,
        "readings": message.readings,
    });
    serde_json::to_writer(&mut *writer, &record)?;
    writer.write_all(b"\n")?;
    Ok(())
}

fn device_timing_metadata(message: &WsTelemetryMessage) -> Option<serde_json::Value> {
    let mut timing = serde_json::Map::new();
    for field in DEVICE_TIMING_FIELDS {
        if let Some(value) = message.extra.get(*field) {
            timing.insert((*field).to_string(), value.clone());
        }
    }
    if timing.is_empty() {
        None
    } else {
        Some(serde_json::Value::Object(timing))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::json_contract::JSON_SCHEMA;
    use serde_json::Value;
    use std::io::{Read, Write};
    use std::net::TcpListener;
    use std::sync::mpsc;
    use std::thread;
    use tungstenite::{accept, Message};

    #[test]
    fn recorder_subscribes_at_requested_rate_and_preserves_it_in_ndjson() {
        assert_eq!(DEFAULT_ADC_RECORD_RATE_HZ, 250);
        for rate_hz in [1, 100, 101, DEFAULT_ADC_RECORD_RATE_HZ] {
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let addr = listener.local_addr().unwrap();
            let server = thread::spawn(move || {
                let (stream, _) = listener.accept().unwrap();
                stream
                    .set_read_timeout(Some(Duration::from_secs(2)))
                    .unwrap();
                stream
                    .set_write_timeout(Some(Duration::from_secs(2)))
                    .unwrap();
                let mut socket = accept(stream).unwrap();
                let request: Value =
                    serde_json::from_str(&socket.read().unwrap().into_text().unwrap()).unwrap();
                let interval_us = 1_000_000 / rate_hz;
                if request.get("batch_size").is_some() {
                    let mut batch = crate::adc::fixture::batch();
                    let mut first = batch["samples"][0].clone();
                    first["sequence"] = json!(1);
                    first["uptime_us"] = json!(interval_us);
                    first["values"][0] = json!(0);
                    let mut second = first.clone();
                    second["sequence"] = json!(2);
                    second["uptime_us"] = json!(interval_us * 2);
                    second["values"][0] = json!(100);
                    batch["samples"] = json!([first, second]);
                    socket
                        .send(Message::Text(batch.to_string().into()))
                        .unwrap();
                } else {
                    for sequence in 1..=2 {
                        let mut readings = crate::adc::fixture::http()["readings"].clone();
                        readings[0]["value"] = json!((sequence - 1) * 100);
                        socket
                            .send(Message::Text(
                                json!({
                                    "type": "telemetry", "topic": "adc", "schema": JSON_SCHEMA,
                                    "sequence": sequence, "uptime_us": interval_us * sequence,
                                    "readings": readings
                                })
                                .to_string()
                                .into(),
                            ))
                            .unwrap();
                    }
                }
                request
            });
            let ws =
                WsClient::with_ws_url(format!("http://{addr}"), format!("ws://{addr}/api/v1/ws/0"));
            let path = temp_file_path(&format!("subscription-{rate_hz}"));
            record_adc_ws_to_file_with_session(&ws, path.to_str().unwrap(), Some(2), rate_hz)
                .unwrap();
            ws.close().unwrap();
            let request = server.join().unwrap();
            assert_eq!(request["type"], "subscribe");
            assert_eq!(request["topic"], "live");
            assert_eq!(request["rate_hz"], rate_hz);
            assert_eq!(
                request.get("batch_size").and_then(Value::as_u64),
                (rate_hz > 100).then_some(18)
            );

            let ndjson = std::fs::read_to_string(&path).unwrap();
            let _ = std::fs::remove_file(&path);
            let records: Vec<Value> = ndjson
                .lines()
                .map(|line| serde_json::from_str(line).unwrap())
                .collect();
            assert_eq!(records.len(), 2);
            for (index, record) in records.iter().enumerate() {
                assert_eq!(record["metadata"]["requested_rate_hz"], request["rate_hz"]);
                assert!(record["metadata"]["requested_rate_hz"].as_i64().unwrap() <= 250);
                assert_eq!(record["sequence"], index + 1);
                assert_eq!(record["readings"][0]["current_ua"], index * 100);
                assert_eq!(record["readings"].as_array().unwrap().len(), 8);
                assert_eq!(record["readings"][4]["voltage_uv"], 2_608_696);
            }
        }
    }

    #[test]
    fn recorder_rejects_invalid_rates_before_creating_a_live_session() {
        for rate_hz in [-1, 0, 251, 1000] {
            let error = record_adc_ws_to_file(
                "not-a-url",
                Duration::ZERO,
                "unused.ndjson",
                Some(1),
                rate_hz,
            )
            .unwrap_err();
            assert_eq!(error.to_string(), "ADC record rate must be in 1..=250 Hz");
        }
    }

    #[test]
    fn telemetry_record_includes_requested_rate_metadata() {
        let path = temp_file_path("rate");
        let mut writer = BufWriter::new(File::create(&path).unwrap());
        let message = WsTelemetryMessage {
            r#type: "telemetry".to_string(),
            topic: "adc".to_string(),
            schema: JSON_SCHEMA.to_string(),
            sequence: Some(7),
            readings: vec![],
            extra: serde_json::Map::new(),
        };

        write_telemetry_record(&mut writer, &message, Duration::from_millis(2), 123, 25).unwrap();
        writer.flush().unwrap();

        drop(writer);
        let data = std::fs::read_to_string(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        let record: Value = serde_json::from_str(data.trim()).unwrap();
        assert_eq!(record["schema"], JSON_SCHEMA);
        assert_eq!(record["type"], "telemetry-record");
        assert_eq!(record["metadata"]["requested_rate_hz"], 25);
        assert!(record["metadata"].get("device_timing").is_none());
        assert_eq!(record["t_mono_ns"], 2_000_000);
        assert_eq!(record["t_unix_ns"], 123);
        assert_eq!(record["sequence"], 7);
    }

    #[test]
    fn telemetry_record_includes_device_timing_when_present() {
        let path = temp_file_path("timing");
        let mut writer = BufWriter::new(File::create(&path).unwrap());
        let mut extra = serde_json::Map::new();
        extra.insert("device_t_mono_us".to_string(), serde_json::json!(42));
        extra.insert("sample_sequence".to_string(), serde_json::json!(99));
        let message = WsTelemetryMessage {
            r#type: "telemetry".to_string(),
            topic: "adc".to_string(),
            schema: JSON_SCHEMA.to_string(),
            sequence: Some(8),
            readings: vec![],
            extra,
        };

        write_telemetry_record(&mut writer, &message, Duration::from_millis(3), 456, 50).unwrap();
        writer.flush().unwrap();

        drop(writer);
        let data = std::fs::read_to_string(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        let record: Value = serde_json::from_str(data.trim()).unwrap();
        assert_eq!(record["metadata"]["requested_rate_hz"], 50);
        assert_eq!(record["metadata"]["device_timing"]["device_t_mono_us"], 42);
        assert_eq!(record["metadata"]["device_timing"]["sample_sequence"], 99);
    }

    #[test]
    fn telemetry_record_preserves_batch_timing_aliases() {
        let path = temp_file_path("batch-aliases");
        let mut writer = BufWriter::new(File::create(&path).unwrap());
        let batch: WsTelemetryBatch = serde_json::from_value(serde_json::json!({
            "type": "telemetry-batch",
            "topic": "adc",
            "schema": JSON_SCHEMA,
                "channels": [{"name": "5v_out", "signal": "S_C_5V", "kind": "current", "unit": "uA"}],
                "samples": [{
                    "sequence": 10,
                    "uptime_us": 1234,
                    "sample_sequence": 210,
                    "device_t_mono_us": 2234,
                    "power_enabled_mask": 1,
                    "values": [456000],
                    "voltages_uv": [0]
                }]
        }))
        .unwrap();
        let message = expand_telemetry_batch(batch).unwrap().remove(0);

        write_telemetry_record(&mut writer, &message, Duration::from_millis(5), 999, 100).unwrap();
        writer.flush().unwrap();
        drop(writer);

        let data = std::fs::read_to_string(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        let record: Value = serde_json::from_str(data.trim()).unwrap();
        let timing = &record["metadata"]["device_timing"];
        assert_eq!(timing["uptime_us"], 1234);
        assert_eq!(timing["sample_sequence"], 210);
        assert_eq!(timing["device_t_mono_us"], 2234);
    }

    #[test]
    fn csv_record_includes_device_time_and_rail_values() {
        let path = temp_file_path("csv");
        let mut writer = BufWriter::new(File::create(&path).unwrap());
        let mut extra = serde_json::Map::new();
        extra.insert("device_t_mono_us".to_string(), serde_json::json!(55));
        let message = WsTelemetryMessage {
            sequence: Some(9),
            readings: vec![crate::adc::AdcReading {
                name: "5v_out".to_string(),
                signal: String::new(),
                raw: None,
                current_valid: None,
                mv: None,
                ma_est: None,
                power_enabled: Some(true),
                kind: crate::adc::AdcKind::Current,
                sensor_channel: String::new(),
                unit: String::new(),
                sensor_value: None,
                value: Some(123_000),
                current_ua: Some(123_000),
                voltage_valid: Some(false),
                voltage_uv: None,
                ..Default::default()
            }],
            extra,
            ..Default::default()
        };

        write_csv_header(&mut writer, &[]).unwrap();
        write_telemetry_csv_row(&mut writer, &message, Duration::from_millis(2), 77, &[]).unwrap();
        writer.flush().unwrap();
        drop(writer);
        let data = std::fs::read_to_string(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        assert!(data.contains("device_t_mono_us"));
        let row: Vec<_> = data.lines().nth(1).unwrap().split(',').collect();
        assert_eq!(row.len(), 16);
        assert_eq!(&row[..6], &["9", "2000000", "77", "55", "true", "123000"]);
        assert!(row[6..].iter().all(|cell| cell.is_empty()));
    }

    #[test]
    fn csv_record_falls_back_to_uptime_when_device_time_is_missing() {
        let path = temp_file_path("csv-uptime");
        let mut writer = BufWriter::new(File::create(&path).unwrap());
        let batch: WsTelemetryBatch = serde_json::from_value(serde_json::json!({
            "type": "telemetry-batch",
            "topic": "adc",
            "schema": JSON_SCHEMA,
                "channels": [{"name": "5v_out", "signal": "S_C_5V", "kind": "current", "unit": "uA"}],
                "samples": [{
                    "sequence": 10,
                    "uptime_us": 1234,
                    "power_enabled_mask": 1,
                    "values": [456000],
                    "voltages_uv": [0]
                }]
        }))
        .unwrap();
        let message = expand_telemetry_batch(batch).unwrap().remove(0);

        write_csv_header(&mut writer, &[]).unwrap();
        write_telemetry_csv_row(&mut writer, &message, Duration::from_millis(3), 88, &[]).unwrap();
        writer.flush().unwrap();
        drop(writer);
        let data = std::fs::read_to_string(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        assert!(data.contains("10,3000000,88,1234,true,456000"));
    }

    #[test]
    fn ndjson_and_csv_retain_all_eight_channels_with_raw_pad_voltage() {
        let path = temp_file_path("adc3");
        let mut writer = BufWriter::new(File::create(&path).unwrap());
        let batch: WsTelemetryBatch = serde_json::from_value(crate::adc::fixture::batch()).unwrap();
        let message = expand_telemetry_batch(batch).unwrap().remove(0);

        write_telemetry_record(&mut writer, &message, Duration::ZERO, 1, 100).unwrap();
        writer.flush().unwrap();
        drop(writer);
        let data = std::fs::read_to_string(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        let record: Value = serde_json::from_str(data.trim()).unwrap();

        assert_eq!(record["readings"].as_array().unwrap().len(), 8);
        assert_eq!(record["readings"][4]["name"], "adc0");
        assert_eq!(record["readings"][4]["voltage_uv"], 2_608_696);
        assert_eq!(record["readings"][4]["note"], "pad voltage");
        assert_eq!(record["readings"][7]["voltage_uv"], 0);

        let csv_path = temp_file_path("adc3-csv");
        let mut csv = BufWriter::new(File::create(&csv_path).unwrap());
        let channels = message
            .readings
            .iter()
            .filter(|reading| reading.kind == AdcKind::Voltage)
            .map(|reading| reading.name.clone())
            .collect::<Vec<_>>();
        write_csv_header(&mut csv, &channels).unwrap();
        write_telemetry_csv_row(&mut csv, &message, Duration::ZERO, 1, &channels).unwrap();
        csv.flush().unwrap();
        drop(csv);
        let csv = std::fs::read_to_string(&csv_path).unwrap();
        let _ = std::fs::remove_file(&csv_path);
        assert_eq!(csv.lines().next(), Some("sequence,t_mono_ns,t_unix_ns,device_t_mono_us,5v_out_power_enabled,5v_out_current_ua,5v_out_voltage_uv,12v_out_power_enabled,12v_out_current_ua,12v_out_voltage_uv,20v_out_power_enabled,20v_out_current_ua,20v_out_voltage_uv,vdd_5v_power_enabled,vdd_5v_current_ua,vdd_5v_voltage_uv,adc0_voltage_uv,adc1_voltage_uv,adc2_voltage_uv,adc3_voltage_uv"));
        assert_eq!(
            csv.lines().nth(1),
            Some("14,0,1,4321,true,100,5000000,true,200,,true,300,20000000,true,400,5100000,2608696,1100000,2200000,0")
        );
        assert!(csv.contains("adc3_voltage_uv"));
    }

    #[test]
    fn csv_distinguishes_unknown_vdd_voltage_from_measured_zero() {
        let path = temp_file_path("csv-voltage-validity");
        let mut writer = BufWriter::new(File::create(&path).unwrap());
        for (valid, voltage_uv) in [(false, None), (false, Some(5_000_000)), (true, Some(0))] {
            let message = WsTelemetryMessage {
                readings: vec![crate::adc::AdcReading {
                    name: "vdd_5v".to_string(),
                    power_enabled: Some(false),
                    current_ua: Some(0),
                    voltage_valid: Some(valid),
                    voltage_uv,
                    ..Default::default()
                }],
                ..Default::default()
            };
            write_telemetry_csv_row(&mut writer, &message, Duration::ZERO, 1, &[]).unwrap();
        }
        writer.flush().unwrap();
        drop(writer);
        let csv = std::fs::read_to_string(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        let rows: Vec<Vec<&str>> = csv.lines().map(|row| row.split(',').collect()).collect();
        assert!(rows.iter().all(|row| row.len() == 16));
        assert_eq!(&rows[0][13..], &["false", "0", ""]);
        assert_eq!(&rows[1][13..], &["false", "0", ""]);
        assert_eq!(&rows[2][13..], &["false", "0", "0"]);
    }

    #[test]
    fn csv_voltage_columns_follow_descriptors_and_keep_missing_cells_empty() {
        let mut message =
            expand_telemetry_batch(serde_json::from_value(crate::adc::fixture::batch()).unwrap())
                .unwrap()
                .remove(0);
        message.readings[4..].rotate_left(2);
        let channels = message
            .readings
            .iter()
            .filter(|reading| reading.kind == AdcKind::Voltage)
            .map(|reading| reading.name.clone())
            .collect::<Vec<_>>();
        let path = temp_file_path("csv-pad-order");
        let mut writer = BufWriter::new(File::create(&path).unwrap());
        write_csv_header(&mut writer, &channels).unwrap();
        write_telemetry_csv_row(&mut writer, &message, Duration::ZERO, 1, &channels).unwrap();
        message.readings.retain(|reading| reading.name != "adc2");
        message
            .readings
            .iter_mut()
            .find(|reading| reading.name == "adc0")
            .unwrap()
            .voltage_uv = None;
        write_telemetry_csv_row(&mut writer, &message, Duration::ZERO, 1, &channels).unwrap();
        writer.flush().unwrap();
        drop(writer);
        let csv = std::fs::read_to_string(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        let rows: Vec<Vec<_>> = csv.lines().map(|line| line.split(',').collect()).collect();
        assert!(rows.iter().all(|row| row.len() == 20));
        assert_eq!(
            &rows[0][16..],
            [
                "adc2_voltage_uv",
                "adc3_voltage_uv",
                "adc0_voltage_uv",
                "adc1_voltage_uv"
            ]
        );
        assert_eq!(&rows[1][16..], ["2200000", "0", "2608696", "1100000"]);
        assert_eq!(&rows[2][16..], ["", "0", "", "1100000"]);
    }

    #[test]
    fn csv_recorder_derives_its_header_from_the_eight_channel_batch() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let server = thread::spawn(move || {
            let (stream, _) = listener.accept().unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(2)))
                .unwrap();
            let mut socket = accept(stream).unwrap();
            let request: Value =
                serde_json::from_str(&socket.read().unwrap().into_text().unwrap()).unwrap();
            assert_eq!(request["batch_size"], 18);
            socket
                .send(Message::Text(
                    crate::adc::fixture::batch().to_string().into(),
                ))
                .unwrap();
        });
        let ws =
            WsClient::with_ws_url(format!("http://{addr}"), format!("ws://{addr}/api/v1/ws/0"));
        let mut path = temp_file_path("csv-eight-channel-session");
        path.set_extension("csv");
        record_adc_ws_to_file_with_session(&ws, path.to_str().unwrap(), Some(1), 250).unwrap();
        ws.close().unwrap();
        server.join().unwrap();
        let csv = std::fs::read_to_string(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        assert!(csv
            .lines()
            .next()
            .unwrap()
            .ends_with(",adc0_voltage_uv,adc1_voltage_uv,adc2_voltage_uv,adc3_voltage_uv"));
        assert!(csv
            .lines()
            .nth(1)
            .unwrap()
            .ends_with(",2608696,1100000,2200000,0"));
    }

    #[test]
    fn csv_missing_rails_and_unknown_fields_are_empty_cells() {
        let path = temp_file_path("csv-unknown-fields");
        let mut writer = BufWriter::new(File::create(&path).unwrap());
        let partial = WsTelemetryMessage {
            readings: vec![
                crate::adc::AdcReading {
                    name: "5v_out".to_string(),
                    power_enabled: Some(false),
                    voltage_valid: Some(true),
                    voltage_uv: Some(0),
                    ..Default::default()
                },
                crate::adc::AdcReading {
                    name: "12v_out".to_string(),
                    ..Default::default()
                },
                crate::adc::AdcReading {
                    name: "vdd_5v".to_string(),
                    current_ua: Some(0),
                    ..Default::default()
                },
            ],
            ..Default::default()
        };
        for message in [WsTelemetryMessage::default(), partial] {
            write_telemetry_csv_row(&mut writer, &message, Duration::ZERO, 1, &[]).unwrap();
        }
        writer.flush().unwrap();
        drop(writer);
        let csv = std::fs::read_to_string(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        let rows: Vec<Vec<_>> = csv.lines().map(|line| line.split(',').collect()).collect();
        assert!(rows.iter().all(|row| row.len() == 16));
        assert!(rows[0][4..].iter().all(|cell| cell.is_empty()));
        assert_eq!(&rows[1][4..7], &["false", "", "0"]);
        assert!(rows[1][7..13].iter().all(|cell| cell.is_empty()));
        assert_eq!(&rows[1][13..], &["", "0", ""]);
    }

    #[test]
    fn telemetry_record_includes_reported_dropped_samples() {
        let path = temp_file_path("dropped");
        let mut writer = BufWriter::new(File::create(&path).unwrap());
        let mut extra = serde_json::Map::new();
        extra.insert("dropped_samples".to_string(), serde_json::json!(3));
        let message = WsTelemetryMessage {
            sequence: Some(9),
            extra,
            ..Default::default()
        };

        write_telemetry_record(&mut writer, &message, Duration::from_millis(4), 789, 100).unwrap();
        writer.flush().unwrap();
        drop(writer);

        let data = std::fs::read_to_string(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        let record: Value = serde_json::from_str(data.trim()).unwrap();
        assert_eq!(record["metadata"]["dropped_samples"], 3);
    }

    #[test]
    fn device_timing_metadata_uses_only_device_fields_from_telemetry() {
        let mut extra = serde_json::Map::new();
        extra.insert("uptime_ms".to_string(), serde_json::json!(42));
        extra.insert("ignored".to_string(), serde_json::json!("host"));
        let message = WsTelemetryMessage {
            extra,
            ..Default::default()
        };

        let timing = device_timing_metadata(&message).unwrap();
        assert_eq!(timing["uptime_ms"], 42);
        assert!(timing.get("ignored").is_none());
    }

    #[test]
    fn record_connect_failure_deletes_live_session() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let (tx, rx) = mpsc::channel();

        thread::spawn(move || {
            let (mut create_stream, _) = listener.accept().unwrap();
            let create_request = read_http_request(&mut create_stream);
            tx.send(create_request).unwrap();
            let create_body = r#"{"ok":true,"schema":"radxa-linkr-debugger.v1","command":"live-sessions","action":"create","session_id":42,"ws_url":"ws://127.0.0.1:0/api/v1/ws/0"}"#;
            write_http_response(&mut create_stream, create_body);

            let (mut delete_stream, _) = listener.accept().unwrap();
            let delete_request = read_http_request(&mut delete_stream);
            tx.send(delete_request).unwrap();
            let delete_body = r#"{"ok":true,"schema":"radxa-linkr-debugger.v1","command":"live-sessions","action":"delete","session_id":42}"#;
            write_http_response(&mut delete_stream, delete_body);
        });

        let path = temp_file_path("connect-cleanup");
        let result = record_adc_ws_to_file(
            &format!("http://{}", addr),
            Duration::from_secs(2),
            path.to_str().unwrap(),
            Some(1),
            10,
        );
        let _ = std::fs::remove_file(&path);

        assert!(result.is_err());
        let create_request = rx.recv_timeout(Duration::from_secs(2)).unwrap();
        let delete_request = rx.recv_timeout(Duration::from_secs(2)).unwrap();
        assert!(
            create_request.starts_with("POST /api/v1/live-sessions HTTP/1.1"),
            "{create_request}"
        );
        assert!(
            delete_request.starts_with("DELETE /api/v1/live-sessions/42 HTTP/1.1"),
            "{delete_request}"
        );
    }

    fn temp_file_path(name: &str) -> std::path::PathBuf {
        let mut path = std::env::temp_dir();
        path.push(format!(
            "radxa-linkr-debugger-recorder-test-{name}-{}.ndjson",
            std::process::id(),
        ));
        path
    }

    fn read_http_request(stream: &mut std::net::TcpStream) -> String {
        let mut buf = [0u8; 2048];
        let n = stream.read(&mut buf).unwrap();
        String::from_utf8_lossy(&buf[..n]).to_string()
    }

    fn write_http_response(stream: &mut std::net::TcpStream, body: &str) {
        let response = format!(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{}",
            body.len(),
            body
        );
        stream.write_all(response.as_bytes()).unwrap();
    }
}
