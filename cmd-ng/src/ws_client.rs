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
    pub sequence: Option<u32>,
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
    pub sequence: Option<u32>,
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
    pub sequence: Option<u32>,
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
}

pub enum WsMessage {
    Snapshot(Box<WsStatusSnapshot>),
    Telemetry(Box<WsTelemetryMessage>),
    Result(WsEnvelope),
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
            "result" => Ok(WsMessage::Result(base)),
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
    }
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
        subscribe_request, WsClient, WsCommandRequest, WsEnvelope, WsMessage, WsStatusSnapshot,
    };
    use crate::json_contract::JSON_SCHEMA;
    use crate::monitoring::{
        BoardMonitoring, MonitoringAvailability, MonitoringCpu, MonitoringHeap, MonitoringRuntime,
        MonitoringTemperature,
    };
    use std::net::TcpListener;
    use std::thread;
    use tungstenite::{accept, Message};

    #[test]
    fn ws_client_close_advances_generation() {
        let client = WsClient::with_ws_url(
            "http://172.29.203.1:8080".to_string(),
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
        let client = WsClient::with_ws_url("http://172.29.203.1:8080".to_string(), ws_url);
        client.connect().unwrap();
        client.send(&subscribe_request(60)).unwrap();
        let raw = rx.recv().unwrap();
        let got: WsCommandRequest = serde_json::from_str(&raw).unwrap();
        assert_eq!(got.message_type, "subscribe");
        assert_eq!(got.topic.as_deref(), Some("live"));
        assert_eq!(got.rate_hz, Some(60));
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
        let client = WsClient::with_ws_url("http://172.29.203.1:8080".to_string(), ws_url);
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
        let client = WsClient::with_ws_url("http://172.29.203.1:8080".to_string(), ws_url);
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
