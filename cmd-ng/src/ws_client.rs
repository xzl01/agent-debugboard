// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

#![allow(dead_code)]

use crate::adc::{transform_readings, AdcReading};
use crate::json_contract::{JsonError, JSON_SCHEMA};
use crate::monitoring::BoardMonitoring;
use anyhow::{anyhow, Result};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use std::net::TcpStream;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;
use tungstenite::stream::MaybeTlsStream;
use tungstenite::{connect, Message, WebSocket};

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct WsEnvelope {
    #[serde(default)]
    pub r#type: String,
    #[serde(default)]
    pub topic: String,
    #[serde(default)]
    pub schema: String,
    pub sequence: Option<u64>,
    #[serde(default)]
    pub id: String,
    #[serde(default)]
    pub command: String,
    pub ok: Option<bool>,
    #[serde(default)]
    pub status: String,
    pub error: Option<JsonError>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct TuiStatusPowerOutput {
    pub name: String,
    pub state: String,
    pub value: i32,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct TuiStatusGpio {
    pub name: String,
    #[serde(default)]
    pub pin: u32,
    #[serde(default)]
    pub value: Option<i32>,
    #[serde(default)]
    pub direction: String,
    #[serde(default)]
    pub note: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct TuiStatusSwitchRoute {
    #[serde(default)]
    pub route: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct TuiStatusSwitches {
    #[serde(default)]
    pub sd: TuiStatusSwitchRoute,
    #[serde(default)]
    pub usb: TuiStatusSwitchRoute,
    #[serde(default)]
    pub vin: TuiStatusSwitchRoute,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct TuiStatusWatchdog {
    pub automatic: bool,
    pub healthy: bool,
    pub supported: bool,
    pub armed: bool,
    pub timeout_ms: u32,
    pub bootloader_on_timeout: bool,
    #[serde(default)]
    pub failing_service: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct WsStatusSnapshot {
    #[serde(default)]
    pub r#type: String,
    #[serde(default)]
    pub topic: String,
    #[serde(default)]
    pub schema: String,
    pub sequence: Option<u64>,
    #[serde(default)]
    pub power_outputs: Vec<TuiStatusPowerOutput>,
    #[serde(default)]
    pub switches: TuiStatusSwitches,
    #[serde(default)]
    pub watchdog: TuiStatusWatchdog,
    #[serde(default)]
    pub gpios: Vec<TuiStatusGpio>,
    #[serde(default)]
    pub board_monitoring: BoardMonitoring,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct WsTelemetryMessage {
    #[serde(default)]
    pub r#type: String,
    #[serde(default)]
    pub topic: String,
    #[serde(default)]
    pub schema: String,
    pub sequence: Option<u64>,
    #[serde(default)]
    pub readings: Vec<AdcReading>,
    #[serde(default, flatten)]
    pub extra: Map<String, Value>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WsCommandRequest {
    #[serde(rename = "type")]
    pub message_type: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub command: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub topic: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub output: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub state: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub route: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub gpio: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub direction: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub value: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub rate_hz: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub batch_size: Option<u8>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WsTelemetryBatchChannel {
    pub name: String,
    #[serde(default)]
    pub signal: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WsTelemetryBatchSample {
    pub sequence: u64,
    pub uptime_us: u64,
    #[serde(default)]
    pub sample_sequence: Option<u64>,
    #[serde(default)]
    pub device_t_mono_us: Option<u64>,
    pub values: Vec<[i32; 4]>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WsTelemetryBatch {
    #[serde(rename = "type")]
    pub message_type: String,
    pub topic: String,
    pub schema: String,
    #[serde(default)]
    pub dropped_samples: u32,
    pub channels: Vec<WsTelemetryBatchChannel>,
    pub samples: Vec<WsTelemetryBatchSample>,
}

pub enum WsMessage {
    Snapshot(Box<WsStatusSnapshot>),
    Telemetry(Box<WsTelemetryMessage>),
    TelemetryBatch(Box<WsTelemetryBatch>),
    Result(Box<WsEnvelope>),
}

pub struct WsClient {
    base_url: String,
    ws_url: Option<String>,
    conn: Mutex<Option<WebSocket<MaybeTlsStream<TcpStream>>>>,
    generation: AtomicU64,
}

impl WsClient {
    pub fn with_ws_url(base_url: String, ws_url: String) -> Self {
        Self {
            base_url,
            ws_url: Some(ws_url),
            conn: Mutex::new(None),
            generation: AtomicU64::new(1),
        }
    }

    pub fn generation(&self) -> u64 {
        self.generation.load(Ordering::SeqCst)
    }

    pub fn is_current_generation(&self, generation: u64) -> bool {
        self.generation() == generation
    }

    pub fn is_connected(&self) -> bool {
        self.conn.lock().expect("lock ws conn").is_some()
    }

    pub fn connect(&self) -> Result<()> {
        let mut guard = self.conn.lock().expect("lock ws conn");
        if guard.is_some() {
            return Ok(());
        }

        let ws_url = self
            .ws_url
            .clone()
            .ok_or_else(|| anyhow!("missing dedicated websocket URL"))?;
        let (socket, _) = connect(&ws_url)?;
        *guard = Some(socket);
        Ok(())
    }

    pub fn close(&self) -> Result<()> {
        let mut guard = self.conn.lock().expect("lock ws conn");
        if let Some(mut socket) = guard.take() {
            let _ = socket.close(None);
        }
        self.generation.fetch_add(1, Ordering::SeqCst);
        Ok(())
    }

    pub fn send(&self, request: &WsCommandRequest) -> Result<()> {
        let mut guard = self.conn.lock().expect("lock ws conn");
        let socket = guard
            .as_mut()
            .ok_or_else(|| anyhow!("websocket not connected"))?;
        socket.send(Message::Text(serde_json::to_string(request)?.into()))?;
        Ok(())
    }

    pub fn recv(&self) -> Result<WsMessage> {
        let mut guard = self.conn.lock().expect("lock ws conn");
        let socket = guard
            .as_mut()
            .ok_or_else(|| anyhow!("websocket not connected"))?;
        let text = read_message_text(socket)?;

        let value: Value = serde_json::from_str(&text)?;
        let base: WsEnvelope = serde_json::from_value(value.clone())?;
        match base.r#type.as_str() {
            "snapshot" => Ok(WsMessage::Snapshot(Box::new(serde_json::from_value(
                value,
            )?))),
            "telemetry" => {
                let mut telemetry: WsTelemetryMessage = serde_json::from_str(&text)?;
                telemetry.readings =
                    transform_readings(telemetry.readings).map_err(|err| anyhow!(err))?;
                Ok(WsMessage::Telemetry(Box::new(telemetry)))
            }
            "telemetry-batch" => Ok(WsMessage::TelemetryBatch(Box::new(serde_json::from_value(
                value,
            )?))),
            "result" => Ok(WsMessage::Result(Box::new(base))),
            "error" => {
                if let Some(error) = base.error {
                    Err(anyhow!("{}: {}", error.code, error.message))
                } else {
                    Err(anyhow!("websocket error"))
                }
            }
            other => Err(anyhow!("unknown websocket message type {:?}", other)),
        }
    }

    pub fn recv_text(&self) -> Result<String> {
        let mut guard = self.conn.lock().expect("lock ws conn");
        let socket = guard
            .as_mut()
            .ok_or_else(|| anyhow!("websocket not connected"))?;
        read_message_text(socket)
    }
}

fn read_message_text(socket: &mut WebSocket<MaybeTlsStream<TcpStream>>) -> Result<String> {
    let message = socket.read()?;
    let text = match message {
        Message::Text(text) => text,
        Message::Binary(data) => String::from_utf8(data.to_vec())?.into(),
        other => return Err(anyhow!("unsupported websocket message {:?}", other)),
    };
    Ok(text.to_string())
}

pub fn subscribe_request(rate_hz: i32) -> WsCommandRequest {
    WsCommandRequest {
        message_type: "subscribe".to_string(),
        command: None,
        topic: Some("live".to_string()),
        output: None,
        name: None,
        state: None,
        route: None,
        gpio: None,
        direction: None,
        value: None,
        rate_hz: Some(rate_hz),
        batch_size: None,
    }
}

pub fn subscribe_batch_request(rate_hz: i32, batch_size: u8) -> WsCommandRequest {
    WsCommandRequest {
        batch_size: Some(batch_size),
        ..subscribe_request(rate_hz)
    }
}

pub fn expand_telemetry_batch(batch: WsTelemetryBatch) -> Result<Vec<WsTelemetryMessage>> {
    let mut messages = Vec::with_capacity(batch.samples.len());

    for (sample_index, sample) in batch.samples.into_iter().enumerate() {
        if sample.values.len() != batch.channels.len() {
            return Err(anyhow!(
                "telemetry batch channel/value count mismatch: {} channels, {} values",
                batch.channels.len(),
                sample.values.len()
            ));
        }

        let readings = batch
            .channels
            .iter()
            .zip(sample.values)
            .map(|(channel, values)| AdcReading {
                name: channel.name.clone(),
                signal: channel.signal.clone(),
                raw: Some(values[1]),
                current_valid: None,
                mv: Some(values[2]),
                ma_est: None,
                power_enabled: Some(values[0] != 0),
                sensor_channel: "current".to_string(),
                unit: "A".to_string(),
                sensor_value: None,
                current_ua: Some(values[3]),
            })
            .collect();
        let sample_sequence = sample.sample_sequence.unwrap_or(sample.sequence);
        let device_t_mono_us = sample.device_t_mono_us.unwrap_or(sample.uptime_us);
        let mut extra = Map::new();
        extra.insert("uptime_us".to_string(), Value::from(sample.uptime_us));
        extra.insert("sample_sequence".to_string(), Value::from(sample_sequence));
        extra.insert(
            "device_t_mono_us".to_string(),
            Value::from(device_t_mono_us),
        );
        if sample_index == 0 && batch.dropped_samples != 0 {
            extra.insert(
                "dropped_samples".to_string(),
                Value::from(batch.dropped_samples),
            );
        }
        messages.push(WsTelemetryMessage {
            r#type: "telemetry".to_string(),
            topic: batch.topic.clone(),
            schema: batch.schema.clone(),
            sequence: Some(sample.sequence),
            readings,
            extra,
        });
    }

    Ok(messages)
}

pub fn subscribe_ok_result() -> WsEnvelope {
    WsEnvelope {
        r#type: "result".to_string(),
        schema: JSON_SCHEMA.to_string(),
        command: "subscribe".to_string(),
        ok: Some(true),
        status: "ok".to_string(),
        ..Default::default()
    }
}

#[cfg(test)]
mod tests {
    use super::{
        expand_telemetry_batch, subscribe_batch_request, subscribe_request, WsClient,
        WsCommandRequest, WsEnvelope, WsMessage, WsStatusSnapshot, WsTelemetryBatch,
    };
    use crate::json_contract::JSON_SCHEMA;
    use crate::monitoring::{
        BoardMonitoring, MonitoringAvailability, MonitoringCpu, MonitoringHeap, MonitoringMemory,
        MonitoringRuntime, MonitoringTemperature,
    };
    use std::net::TcpListener;
    use std::thread;
    use tungstenite::{accept, Message};

    #[test]
    fn status_switches_accept_optional_vin_route() {
        let legacy: WsStatusSnapshot =
            serde_json::from_str(r#"{"switches":{"sd":{"route":"target"},"usb":{"route":"pc"}}}"#)
                .unwrap();
        assert!(legacy.switches.vin.route.is_empty());

        let current: WsStatusSnapshot = serde_json::from_str(
            r#"{"switches":{"sd":{"route":"target"},"usb":{"route":"pc"},"vin":{"route":"1.8v"}}}"#,
        )
        .unwrap();
        assert_eq!(current.switches.vin.route, "1.8v");
    }

    #[test]
    fn status_snapshot_accepts_optional_memory_monitoring() {
        let old: WsStatusSnapshot = serde_json::from_str(
            r#"{
                "type":"snapshot",
                "topic":"status",
                "schema":"radxa-linkr-debugger.v1",
                "board_monitoring":{
                    "heap":{"available":true,"allocated_bytes":2048,"total_bytes":8192}
                }
            }"#,
        )
        .unwrap();
        assert!(old.board_monitoring.memory.is_none());
        assert_eq!(old.board_monitoring.heap.allocated_bytes, Some(2048));

        let new: WsStatusSnapshot = serde_json::from_str(
            r#"{
                "type":"snapshot",
                "topic":"status",
                "schema":"radxa-linkr-debugger.v1",
                "board_monitoring":{
                    "memory":{
                        "available":true,
                        "reason":"",
                        "source":"zephyr",
                        "coverage":"heap_and_stacks",
                        "pressure_pct_x100":4250,
                        "limiting_component":"thread_stack",
                        "limiting_name":"main",
                        "system_heap_pressure_pct_x100":3000,
                        "current_pressure":{
                            "available":true,
                            "reason":"",
                            "coverage":"heap_and_stacks",
                            "pressure_pct_x100":3000,
                            "limiting_component":"system_heap",
                            "limiting_name":"",
                            "tie_count":1
                        },
                        "peak_pressure":{
                            "available":true,
                            "reason":"",
                            "coverage":"heap_and_stacks",
                            "pressure_pct_x100":4250,
                            "limiting_component":"thread_stack",
                            "limiting_name":"main",
                            "tie_count":1,
                            "since":"boot"
                        },
                        "physical":{
                            "total_bytes":270336,
                            "image_reserved_bytes":98304,
                            "reserved_pct_x100":3721
                        },
                        "stacks":{
                            "thread_count":7,
                            "measured_count":6,
                            "error_count":1,
                            "total_bytes":12288,
                            "used_high_water_bytes":4096,
                            "max_pressure_pct_x100":4250,
                            "max_pressure_thread":"main"
                        }
                    }
                }
            }"#,
        )
        .unwrap();
        let memory = new.board_monitoring.memory.unwrap();
        assert_eq!(memory.source, "zephyr");
        assert_eq!(memory.pressure_pct_x100, Some(4250));
        assert_eq!(memory.limiting_component, "thread_stack");
        let current = memory.current_pressure.unwrap();
        assert_eq!(current.pressure_pct_x100, Some(3000));
        assert_eq!(current.limiting_component, "system_heap");
        let peak = memory.peak_pressure.unwrap();
        assert_eq!(peak.pressure_pct_x100, Some(4250));
        assert_eq!(peak.since.as_deref(), Some("boot"));
        assert_eq!(memory.physical.total_bytes, Some(270336));
        assert_eq!(memory.physical.image_reserved_bytes, Some(98304));
        assert_eq!(memory.stacks.measured_count, Some(6));
    }

    #[test]
    fn ws_client_close_advances_generation() {
        let client = WsClient::with_ws_url(
            "http://172.29.203.1".to_string(),
            "ws://example.invalid".to_string(),
        );
        let before = client.generation();
        client.close().unwrap();
        let after = client.generation();
        assert!(after > before);
        assert!(!client.is_current_generation(before));
        assert!(client.is_current_generation(after));
    }

    #[test]
    fn ws_client_connect_and_send() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let (tx, rx) = std::sync::mpsc::channel();
        thread::spawn(move || {
            let (stream, _) = listener.accept().unwrap();
            let mut socket = accept(stream).unwrap();
            let msg = socket.read().unwrap();
            let text = match msg {
                Message::Text(text) => text.to_string(),
                other => panic!("unexpected message: {other:?}"),
            };
            tx.send(text).unwrap();
            socket
                .send(Message::Text(
                    serde_json::to_string(&WsStatusSnapshot {
                        r#type: "snapshot".to_string(),
                        topic: "status".to_string(),
                        schema: JSON_SCHEMA.to_string(),
                        sequence: Some(1),
                        board_monitoring: BoardMonitoring {
                            temperature: MonitoringTemperature {
                                availability: MonitoringAvailability {
                                    available: false,
                                    reason: "no_zephyr_temperature_device".to_string(),
                                    error: None,
                                },
                                ..Default::default()
                            },
                            heap: MonitoringHeap {
                                availability: MonitoringAvailability {
                                    available: false,
                                    reason: "runtime_stats_disabled".to_string(),
                                    error: None,
                                },
                                ..Default::default()
                            },
                            memory: Some(MonitoringMemory {
                                availability: MonitoringAvailability {
                                    available: true,
                                    reason: String::new(),
                                    error: None,
                                },
                                pressure_pct_x100: Some(4250),
                                limiting_component: "thread_stack".to_string(),
                                limiting_name: "main".to_string(),
                                ..Default::default()
                            }),
                            runtime: MonitoringRuntime {
                                availability: MonitoringAvailability {
                                    available: true,
                                    reason: String::new(),
                                    error: None,
                                },
                                uptime_ms: Some(12345),
                                uptime_seconds: Some(12),
                            },
                            cpu: MonitoringCpu {
                                availability: MonitoringAvailability {
                                    available: false,
                                    reason: "thread_runtime_stats_disabled".to_string(),
                                    error: None,
                                },
                                ..Default::default()
                            },
                        },
                        ..Default::default()
                    })
                    .unwrap()
                    .into(),
                ))
                .unwrap();
        });

        let ws_url = format!("ws://{}", addr);
        let client = WsClient::with_ws_url("http://172.29.203.1".to_string(), ws_url);
        client.connect().unwrap();
        client.send(&subscribe_request(60)).unwrap();
        let raw = rx.recv().unwrap();
        let got: WsCommandRequest = serde_json::from_str(&raw).unwrap();
        assert_eq!(got.message_type, "subscribe");
        assert_eq!(got.topic.as_deref(), Some("live"));
        assert_eq!(got.rate_hz, Some(60));
        assert_eq!(got.batch_size, None);
    }

    #[test]
    fn expands_telemetry_batch_into_individual_samples() {
        let batch: WsTelemetryBatch = serde_json::from_str(
            r#"{
                "type":"telemetry-batch",
                "topic":"adc",
                "schema":"radxa-linkr-debugger.v1",
                "dropped_samples":2,
                "channels":[{"name":"5v_out","signal":"S_C_5V"}],
                "samples":[
                    {"sequence":10,"uptime_us":1000,"sample_sequence":110,"device_t_mono_us":1001,"values":[[1,12,34,56]]},
                    {"sequence":11,"uptime_us":2000,"sample_sequence":111,"device_t_mono_us":2001,"values":[[0,13,35,57]]}
                ]
            }"#,
        )
        .unwrap();

        let messages = expand_telemetry_batch(batch).unwrap();
        assert_eq!(messages.len(), 2);
        assert_eq!(messages[0].sequence, Some(10));
        assert_eq!(messages[0].readings[0].name, "5v_out");
        assert_eq!(messages[0].readings[0].power_enabled, Some(true));
        assert_eq!(messages[0].readings[0].raw, Some(12));
        assert_eq!(messages[0].readings[0].current_ua, Some(56));
        assert_eq!(messages[0].extra["uptime_us"], 1000);
        assert_eq!(messages[0].extra["sample_sequence"], 110);
        assert_eq!(messages[0].extra["device_t_mono_us"], 1001);
        assert_eq!(messages[0].extra["dropped_samples"], 2);
        assert_eq!(messages[1].sequence, Some(11));
        assert_eq!(messages[1].extra["sample_sequence"], 111);
        assert_eq!(messages[1].extra["device_t_mono_us"], 2001);
        assert!(messages[1].extra.get("dropped_samples").is_none());

        let request = subscribe_batch_request(1000, 10);
        assert_eq!(request.rate_hz, Some(1000));
        assert_eq!(request.batch_size, Some(10));
    }

    #[test]
    fn expands_old_telemetry_batch_using_legacy_timing_fields() {
        let batch: WsTelemetryBatch = serde_json::from_str(
            r#"{
                "type":"telemetry-batch",
                "topic":"adc",
                "schema":"radxa-linkr-debugger.v1",
                "channels":[{"name":"5v_out","signal":"S_C_5V"}],
                "samples":[{"sequence":12,"uptime_us":3000,"values":[[1,14,36,58]]}]
            }"#,
        )
        .unwrap();

        let messages = expand_telemetry_batch(batch).unwrap();
        assert_eq!(messages.len(), 1);
        assert_eq!(messages[0].sequence, Some(12));
        assert_eq!(messages[0].extra["uptime_us"], 3000);
        assert_eq!(messages[0].extra["sample_sequence"], 12);
        assert_eq!(messages[0].extra["device_t_mono_us"], 3000);
    }

    #[test]
    fn recv_parses_result_message() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        thread::spawn(move || {
            let (stream, _) = listener.accept().unwrap();
            let mut socket = accept(stream).unwrap();
            socket
                .send(Message::Text(
                    serde_json::to_string(&WsEnvelope {
                        r#type: "result".to_string(),
                        schema: JSON_SCHEMA.to_string(),
                        command: "power_set".to_string(),
                        ok: Some(true),
                        status: "ok".to_string(),
                        ..Default::default()
                    })
                    .unwrap()
                    .into(),
                ))
                .unwrap();
        });

        let ws_url = format!("ws://{}", addr);
        let client = WsClient::with_ws_url("http://172.29.203.1".to_string(), ws_url);
        client.connect().unwrap();
        match client.recv().unwrap() {
            WsMessage::Result(result) => {
                assert_eq!(result.command, "power_set");
                assert_eq!(result.status, "ok");
            }
            _ => panic!("expected result message"),
        }
    }

    #[test]
    fn recv_parses_telemetry_without_http_envelope_fields() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        thread::spawn(move || {
            let (stream, _) = listener.accept().unwrap();
            let mut socket = accept(stream).unwrap();
            socket
                .send(Message::Text(
                    serde_json::json!({
                        "type": "telemetry",
                        "topic": "live",
                        "schema": JSON_SCHEMA,
                        "sequence": 1,
                        "readings": [{
                            "name": "5v_out",
                            "signal": "S_C_5V",
                            "power_enabled": true,
                            "sensor_channel": "current",
                            "unit": "A",
                            "sensor_value": {"val1": 0, "val2": 850000},
                            "current_ua": 850000
                        }]
                    })
                    .to_string()
                    .into(),
                ))
                .unwrap();
        });

        let ws_url = format!("ws://{}", addr);
        let client = WsClient::with_ws_url("http://172.29.203.1".to_string(), ws_url);
        client.connect().unwrap();
        match client.recv().unwrap() {
            WsMessage::Telemetry(message) => {
                assert_eq!(message.readings.len(), 1);
                assert_eq!(message.readings[0].current_ua, Some(850000));
                assert_eq!(message.readings[0].ma_est, None);
            }
            _ => panic!("expected telemetry message"),
        }
    }
}
