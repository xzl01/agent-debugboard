use crate::client::{BoardClient, BoardRequest};
use crate::json_contract::JSON_SCHEMA;
use crate::ws_client::{subscribe_request, WsClient, WsTelemetryMessage};
use anyhow::{anyhow, Result};
use reqwest::Method;
use serde::Deserialize;
use serde_json::json;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::Path;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

pub const DEFAULT_ADC_RECORD_RATE_HZ: i32 = 1000;

const DEVICE_TIMING_FIELDS: &[&str] = &[
    "device_t_mono_ns",
    "device_t_unix_ns",
    "device_timestamp_ns",
    "sample_t_mono_ns",
    "sample_t_unix_ns",
    "sample_timestamp_ns",
    "timestamp_ns",
    "uptime_ns",
    "uptime_us",
    "uptime_ms",
    "uptime_seconds",
];

#[derive(Debug, Clone, Deserialize)]
struct LiveSessionCreateResponse {
    ok: bool,
    #[serde(default)]
    ws_url: String,
}

fn request_live_session_ws_url(base_url: &str, timeout: Duration) -> Result<String> {
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
    Ok(response.ws_url)
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
    let ws_url = request_live_session_ws_url(base_url, timeout)?;
    let ws_client = WsClient::with_ws_url(base_url.to_string(), ws_url);
    ws_client.connect()?;
    ws_client.send(&subscribe_request(requested_rate_hz))?;

    let file = File::create(Path::new(output_path))?;
    let mut writer = BufWriter::with_capacity(1024 * 1024, file);
    let started = Instant::now();
    let mut written = 0usize;

    loop {
        if let Some(limit) = max_samples {
            if written >= limit {
                break;
            }
        }

        let elapsed = started.elapsed();
        let unix_ns = unix_time_ns();
        let text = ws_client.recv_text()?;
        let value: serde_json::Value = serde_json::from_str(&text)?;
        if value.get("type").and_then(|v| v.as_str()) == Some("telemetry") {
            let mut message: WsTelemetryMessage = serde_json::from_value(value)?;
            message.readings =
                crate::adc::transform_readings(message.readings).map_err(|e| anyhow!(e))?;
            write_telemetry_record(&mut writer, &message, elapsed, unix_ns, requested_rate_hz)?;
            written += 1;
        }
    }

    writer.flush()?;
    ws_client.close()?;
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

        write_telemetry_record(&mut writer, &message, Duration::from_millis(2), 123, 250).unwrap();
        writer.flush().unwrap();

        drop(writer);
        let data = std::fs::read_to_string(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        let record: Value = serde_json::from_str(data.trim()).unwrap();
        assert_eq!(record["schema"], JSON_SCHEMA);
        assert_eq!(record["type"], "telemetry-record");
        assert_eq!(record["metadata"]["requested_rate_hz"], 250);
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
        extra.insert("uptime_ms".to_string(), serde_json::json!(42));
        let message = WsTelemetryMessage {
            r#type: "telemetry".to_string(),
            topic: "adc".to_string(),
            schema: JSON_SCHEMA.to_string(),
            sequence: Some(8),
            readings: vec![],
            extra,
        };

        write_telemetry_record(&mut writer, &message, Duration::from_millis(3), 456, 500).unwrap();
        writer.flush().unwrap();

        drop(writer);
        let data = std::fs::read_to_string(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        let record: Value = serde_json::from_str(data.trim()).unwrap();
        assert_eq!(record["metadata"]["requested_rate_hz"], 500);
        assert_eq!(record["metadata"]["device_timing"]["uptime_ms"], 42);
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

    fn temp_file_path(name: &str) -> std::path::PathBuf {
        let mut path = std::env::temp_dir();
        path.push(format!(
            "agent-debugboard-recorder-test-{name}-{}.ndjson",
            std::process::id(),
        ));
        path
    }
}
