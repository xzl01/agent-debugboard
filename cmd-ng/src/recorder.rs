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
) -> Result<()> {
    let ws_url = request_live_session_ws_url(base_url, timeout)?;
    let ws_client = WsClient::with_ws_url(base_url.to_string(), ws_url);
    ws_client.connect()?;
    ws_client.send(&subscribe_request(1000))?;

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
            write_telemetry_record(&mut writer, &message, elapsed, unix_ns)?;
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
) -> Result<()> {
    let record = json!({
        "schema": JSON_SCHEMA,
        "type": "telemetry-record",
        "t_mono_ns": elapsed.as_nanos(),
        "t_unix_ns": unix_ns,
        "sequence": message.sequence,
        "readings": message.readings,
    });
    serde_json::to_writer(&mut *writer, &record)?;
    writer.write_all(b"\n")?;
    Ok(())
}
