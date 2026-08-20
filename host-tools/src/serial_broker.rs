use std::{
    collections::{HashMap, HashSet},
    sync::Arc,
    time::{Duration, Instant},
};

use axum::extract::ws::{Message, WebSocket};
use base64::{engine::general_purpose::STANDARD as BASE64, Engine};
use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use tokio::{
    io::{AsyncRead, AsyncReadExt, AsyncWriteExt, ReadHalf, WriteHalf},
    sync::{mpsc, Mutex},
    task::JoinHandle,
};
use tokio_serial::{SerialPortBuilderExt, SerialPortInfo, SerialPortType, SerialStream};
use tracing::debug;
use uuid::Uuid;

use crate::serial_log::SerialLogService;

pub const PROTOCOL: &str = "linkr-serial-broker.v1";
const CH347_VENDOR_ID: u16 = 0x1a86;
const MIN_BAUD: u64 = 300;
const MAX_BAUD: u64 = 4_000_000;
const MAX_REDACTION_BUFFER_BYTES: usize = 65_536;
const REDACTION_SUFFIX_BYTES: usize = 512;
const SERIAL_READ_BUFFER_BYTES: usize = 16 * 1024;
const SERIAL_FORWARD_MAX_BYTES: usize = 16 * 1024;
const SERIAL_FORWARD_FLUSH_INTERVAL: Duration = Duration::from_millis(2);

type PeerId = String;
type PeerSender = mpsc::UnboundedSender<Message>;

struct SerialReadBatch {
    bytes: Vec<u8>,
    eof: bool,
    error: Option<std::io::Error>,
}

async fn read_serial_batch<R>(
    reader: &mut R,
    scratch: &mut [u8],
    flush_interval: Duration,
    max_bytes: usize,
) -> SerialReadBatch
where
    R: AsyncRead + Unpin,
{
    assert!(!scratch.is_empty());
    assert!(max_bytes > 0);

    let first_limit = scratch.len().min(max_bytes);
    let first_count = match reader.read(&mut scratch[..first_limit]).await {
        Ok(0) => {
            return SerialReadBatch {
                bytes: Vec::new(),
                eof: true,
                error: None,
            };
        }
        Ok(count) => count,
        Err(error) => {
            return SerialReadBatch {
                bytes: Vec::new(),
                eof: false,
                error: Some(error),
            };
        }
    };

    let mut bytes = Vec::with_capacity(max_bytes.min(first_count.saturating_mul(4)));
    bytes.extend_from_slice(&scratch[..first_count]);
    let deadline = tokio::time::Instant::now() + flush_interval;
    let mut eof = false;
    let mut error = None;

    while bytes.len() < max_bytes {
        let read_limit = scratch.len().min(max_bytes - bytes.len());
        match tokio::time::timeout_at(deadline, reader.read(&mut scratch[..read_limit])).await {
            Ok(Ok(0)) => {
                eof = true;
                break;
            }
            Ok(Ok(count)) => bytes.extend_from_slice(&scratch[..count]),
            Ok(Err(read_error)) => {
                error = Some(read_error);
                break;
            }
            Err(_) => break,
        }
    }

    SerialReadBatch { bytes, eof, error }
}

#[derive(Debug)]
struct ObserverRedaction {
    owner: PeerId,
    token: String,
    pending: String,
}

impl ObserverRedaction {
    fn new(owner: PeerId, token: String) -> Self {
        Self {
            owner,
            token,
            pending: String::new(),
        }
    }

    fn push(&mut self, text: &str) -> String {
        self.pending.push_str(text);
        let mut visible = String::new();
        while let Some(end) = self.pending.find('\n') {
            let line = self.pending.drain(..=end).collect::<String>();
            visible.push_str(&redact_internal_shell_line(&line, &self.token));
        }

        if self.pending.len() > MAX_REDACTION_BUFFER_BYTES {
            let mut split = self.pending.len().saturating_sub(REDACTION_SUFFIX_BYTES);
            while split > 0 && !self.pending.is_char_boundary(split) {
                split -= 1;
            }
            visible.push_str(&self.pending.drain(..split).collect::<String>());
        }
        visible
    }

    fn finish(mut self) -> String {
        redact_internal_shell_line(&std::mem::take(&mut self.pending), &self.token)
    }
}

pub(crate) fn redact_internal_shell_text(text: &str, token: &str) -> String {
    text.split_inclusive('\n')
        .map(|line| redact_internal_shell_line(line, token))
        .collect()
}

fn redact_internal_shell_line(line: &str, token: &str) -> String {
    let variable = format!("__linkr_rc_{token}");
    if line.contains(&variable) {
        return String::new();
    }

    let marker = format!("__LINKR_RC_{token}__");
    let Some(marker_start) = line.find(&marker) else {
        return line.to_owned();
    };
    let prefix = &line[..marker_start];
    if prefix.is_empty() {
        return String::new();
    }
    let ending = if line.ends_with("\r\n") {
        "\r\n"
    } else if line.ends_with('\n') {
        "\n"
    } else {
        ""
    };
    format!("{prefix}{ending}")
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
enum Channel {
    Uart0,
    Uart1,
}

impl Channel {
    fn parse(value: Option<&str>) -> Option<Self> {
        match value {
            Some("uart0") => Some(Self::Uart0),
            Some("uart1") => Some(Self::Uart1),
            _ => None,
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::Uart0 => "uart0",
            Self::Uart1 => "uart1",
        }
    }

    fn preferred_suffix(self) -> &'static str {
        match self {
            Self::Uart0 => "D1",
            Self::Uart1 => "D3",
        }
    }

    fn fallback_index(self) -> usize {
        match self {
            Self::Uart0 => 0,
            Self::Uart1 => 1,
        }
    }
}

struct ChannelState {
    channel: Channel,
    path: Option<String>,
    baud: Option<u32>,
    writer: Option<WriteHalf<SerialStream>>,
    reader_task: Option<JoinHandle<()>>,
    idle_task: Option<JoinHandle<()>>,
    subscribers: HashMap<PeerId, PeerSender>,
    owner: Option<(PeerId, String)>,
    observer_redaction: Option<ObserverRedaction>,
    sequence: u64,
    log_session: Option<Uuid>,
}

impl ChannelState {
    fn new(channel: Channel) -> Self {
        Self {
            channel,
            path: None,
            baud: None,
            writer: None,
            reader_task: None,
            idle_task: None,
            subscribers: HashMap::new(),
            owner: None,
            observer_redaction: None,
            sequence: 0,
            log_session: None,
        }
    }

    fn connected(&self) -> bool {
        self.writer.is_some()
    }

    fn cancel_idle_close(&mut self) {
        if let Some(task) = self.idle_task.take() {
            task.abort();
        }
    }

    fn close_port(&mut self) -> Option<Uuid> {
        self.writer = None;
        self.path = None;
        self.baud = None;
        self.owner = None;
        self.observer_redaction = None;
        if let Some(task) = self.reader_task.take() {
            task.abort();
        }
        self.log_session.take()
    }
}

#[derive(Clone)]
pub struct SerialBroker {
    channels: Arc<HashMap<Channel, Arc<Mutex<ChannelState>>>>,
    peer_subscriptions: Arc<Mutex<HashMap<PeerId, HashSet<Channel>>>>,
    idle_timeout: Duration,
    started: Instant,
    serial_log: SerialLogService,
}

impl SerialBroker {
    pub fn new(idle_timeout: Duration, serial_log: SerialLogService) -> Self {
        let channels = HashMap::from([
            (
                Channel::Uart0,
                Arc::new(Mutex::new(ChannelState::new(Channel::Uart0))),
            ),
            (
                Channel::Uart1,
                Arc::new(Mutex::new(ChannelState::new(Channel::Uart1))),
            ),
        ]);
        Self {
            channels: Arc::new(channels),
            peer_subscriptions: Arc::new(Mutex::new(HashMap::new())),
            idle_timeout,
            started: Instant::now(),
            serial_log,
        }
    }

    pub async fn serve_socket(&self, socket: WebSocket) {
        let peer_id = format!("host-{}", Uuid::new_v4());
        let (mut ws_sink, mut ws_stream) = socket.split();
        let (tx, mut rx) = mpsc::unbounded_channel::<Message>();
        let writer = tokio::spawn(async move {
            while let Some(frame) = rx.recv().await {
                if ws_sink.send(frame).await.is_err() {
                    break;
                }
            }
        });

        let _ = tx.send(frame(
            "hello",
            json!({
                "server": "radxa-linkr-debugger-serial-broker",
                "client_id": peer_id,
                "capabilities": {
                    "channels": ["uart0", "uart1"],
                    "encodings": ["utf8", "base64"],
                    "shared_read": true,
                    "ordered_write": true,
                    "exclusive_write": true,
                    "observer_redaction": "line_token"
                }
            }),
        ));

        while let Some(message) = ws_stream.next().await {
            let Ok(message) = message else { break };
            match message {
                Message::Text(text) => self.dispatch(&peer_id, &tx, text.as_str()).await,
                Message::Binary(bytes) => match std::str::from_utf8(&bytes) {
                    Ok(text) => self.dispatch(&peer_id, &tx, text).await,
                    Err(_) => send_error(
                        &tx,
                        "invalid_json",
                        "broker request must be valid UTF-8 JSON",
                        None,
                        None,
                        false,
                    ),
                },
                Message::Close(_) => break,
                Message::Ping(_) | Message::Pong(_) => {}
            }
        }

        self.disconnect_peer(&peer_id).await;
        writer.abort();
    }

    async fn dispatch(&self, peer_id: &str, sender: &PeerSender, raw: &str) {
        let request = match parse_request(raw) {
            Ok(request) => request,
            Err(error) => {
                send_error(
                    sender,
                    &error.code,
                    &error.message,
                    error.request_id.as_deref(),
                    None,
                    false,
                );
                return;
            }
        };
        match request.kind.as_str() {
            "open" => self.open(peer_id, sender, request).await,
            "write" => self.write(peer_id, sender, request).await,
            "claim" => self.claim(peer_id, sender, request).await,
            "release" => self.release(peer_id, sender, request).await,
            "status" => self.status(sender, request).await,
            "close" => self.close(peer_id, sender, request, true).await,
            _ => unreachable!("request parser only returns supported request types"),
        }
    }

    async fn open(&self, peer_id: &str, sender: &PeerSender, request: ClientRequest) {
        let channel = request.channel;
        let baud = request.baud.expect("validated open request has baud");
        let state_ref = self.channel(channel);
        let mut state = state_ref.lock().await;
        state.cancel_idle_close();

        if let Some(current) = state.baud {
            if current != baud {
                send_error(
                    sender,
                    "baud_conflict",
                    &format!("{} is already open at {current} baud", channel.as_str()),
                    request.request_id.as_deref(),
                    Some(channel),
                    false,
                );
                return;
            }
        }

        if !state.connected() {
            let port = match find_serial_port(channel).await {
                Ok(Some(port)) => port,
                Ok(None) => {
                    send_error(
                        sender,
                        "serial_not_found",
                        &format!(
                            "No CH347F {} serial port found",
                            channel.as_str().to_uppercase()
                        ),
                        request.request_id.as_deref(),
                        Some(channel),
                        true,
                    );
                    return;
                }
                Err(error) => {
                    send_error(
                        sender,
                        "serial_open_failed",
                        &error,
                        request.request_id.as_deref(),
                        Some(channel),
                        true,
                    );
                    return;
                }
            };
            let serial_builder = tokio_serial::new(&port.port_name, baud);
            // Request exclusivity on the builder so it is applied atomically
            // during open. Do not call `set_exclusive(true)` again afterwards:
            // the macOS CH347 driver rejects a repeated TIOCEXCL with EBUSY
            // even when this process already owns the port.
            #[cfg(unix)]
            let serial_builder = serial_builder.exclusive(true);
            let serial = match serial_builder.open_native_async() {
                Ok(serial) => serial,
                Err(error) => {
                    send_error(
                        sender,
                        "serial_open_failed",
                        &error.to_string(),
                        request.request_id.as_deref(),
                        Some(channel),
                        true,
                    );
                    return;
                }
            };
            let (reader, writer) = tokio::io::split(serial);
            state.path = Some(port.port_name.clone());
            state.baud = Some(baud);
            state.writer = Some(writer);
            state.log_session =
                self.serial_log
                    .start_session(channel.as_str(), &port.port_name, baud);
            let broker = self.clone();
            let log_session = state.log_session;
            state.reader_task = Some(tokio::spawn(async move {
                broker.read_serial(channel, reader, log_session).await;
            }));
        }

        state.subscribers.insert(peer_id.to_owned(), sender.clone());
        drop(state);
        self.peer_subscriptions
            .lock()
            .await
            .entry(peer_id.to_owned())
            .or_default()
            .insert(channel);

        let state = state_ref.lock().await;
        let _ = sender.send(frame(
            "opened",
            json!({
                "request_id": request.request_id,
                "channel": channel.as_str(),
                "path": state.path,
                "baud": state.baud,
                "shared": true
            }),
        ));
        publish_status(&state, None);
    }

    async fn write(&self, peer_id: &str, sender: &PeerSender, request: ClientRequest) {
        let channel = request.channel;
        let state_ref = self.channel(channel);
        let mut state = state_ref.lock().await;
        if !state.subscribers.contains_key(peer_id) || !state.connected() {
            send_error(
                sender,
                "serial_not_open",
                &format!("{} is not open for this client", channel.as_str()),
                request.request_id.as_deref(),
                Some(channel),
                true,
            );
            return;
        }
        if let Some((owner, label)) = &state.owner {
            if owner != peer_id {
                send_error(
                    sender,
                    "serial_busy",
                    &format!("{} write access is owned by {label}", channel.as_str()),
                    request.request_id.as_deref(),
                    Some(channel),
                    true,
                );
                return;
            }
        }
        if let Some(token) = request.observer_redaction_token.as_ref() {
            if !state
                .owner
                .as_ref()
                .is_some_and(|(owner, _)| owner == peer_id)
            {
                send_error(
                    sender,
                    "serial_claim_required",
                    "observer redaction requires exclusive write ownership",
                    request.request_id.as_deref(),
                    Some(channel),
                    false,
                );
                return;
            }
            if state.observer_redaction.is_some() {
                send_error(
                    sender,
                    "serial_redaction_active",
                    "an observer redaction operation is already active",
                    request.request_id.as_deref(),
                    Some(channel),
                    true,
                );
                return;
            }
            state.observer_redaction =
                Some(ObserverRedaction::new(peer_id.to_owned(), token.to_owned()));
        }
        let Some(bytes) = request.data else {
            send_error(
                sender,
                "invalid_write",
                "write data is missing",
                request.request_id.as_deref(),
                Some(channel),
                false,
            );
            return;
        };
        let Some(writer) = state.writer.as_mut() else {
            send_error(
                sender,
                "serial_not_open",
                &format!("{} is not open", channel.as_str()),
                request.request_id.as_deref(),
                Some(channel),
                true,
            );
            return;
        };
        if let Err(error) = writer.write_all(&bytes).await {
            send_error(
                sender,
                "serial_write_failed",
                &error.to_string(),
                request.request_id.as_deref(),
                Some(channel),
                true,
            );
            return;
        }
        if let Err(error) = writer.flush().await {
            send_error(
                sender,
                "serial_write_failed",
                &error.to_string(),
                request.request_id.as_deref(),
                Some(channel),
                true,
            );
            return;
        }
        let _ = sender.send(frame(
            "write_ack",
            json!({"request_id": request.request_id, "channel": channel.as_str(), "bytes": bytes.len()}),
        ));
    }

    async fn claim(&self, peer_id: &str, sender: &PeerSender, request: ClientRequest) {
        let channel = request.channel;
        let state_ref = self.channel(channel);
        let mut state = state_ref.lock().await;
        if !state.subscribers.contains_key(peer_id) || !state.connected() {
            send_error(
                sender,
                "serial_not_open",
                &format!("{} is not open for this client", channel.as_str()),
                request.request_id.as_deref(),
                Some(channel),
                true,
            );
            return;
        }
        if let Some((owner, label)) = &state.owner {
            if owner != peer_id {
                send_error(
                    sender,
                    "serial_busy",
                    &format!("{} is already owned by {label}", channel.as_str()),
                    request.request_id.as_deref(),
                    Some(channel),
                    true,
                );
                return;
            }
        }
        let owner_label = request.owner.unwrap_or_else(|| "automation".to_owned());
        state.owner = Some((peer_id.to_owned(), owner_label.clone()));
        let _ = sender.send(frame(
            "claimed",
            json!({"request_id": request.request_id, "channel": channel.as_str(), "owner": owner_label}),
        ));
        publish_status(&state, None);
    }

    async fn release(&self, peer_id: &str, sender: &PeerSender, request: ClientRequest) {
        let state_ref = self.channel(request.channel);
        let mut state = state_ref.lock().await;
        if state
            .owner
            .as_ref()
            .is_some_and(|(owner, _)| owner == peer_id)
        {
            self.finish_observer_redaction(&mut state);
            state.owner = None;
        }
        let _ = sender.send(frame(
            "released",
            json!({"request_id": request.request_id, "channel": request.channel.as_str()}),
        ));
        publish_status(&state, None);
    }

    async fn status(&self, sender: &PeerSender, request: ClientRequest) {
        let state_ref = self.channel(request.channel);
        let state = state_ref.lock().await;
        let _ = sender.send(status_frame(&state, request.request_id.as_deref()));
    }

    async fn close(
        &self,
        peer_id: &str,
        sender: &PeerSender,
        request: ClientRequest,
        immediate: bool,
    ) {
        let channel = request.channel;
        let state_ref = self.channel(channel);
        let mut state = state_ref.lock().await;
        if state
            .owner
            .as_ref()
            .is_some_and(|(owner, _)| owner == peer_id)
        {
            self.finish_observer_redaction(&mut state);
            state.owner = None;
        }
        state.subscribers.remove(peer_id);
        let _ = sender.send(frame(
            "closed",
            json!({"request_id": request.request_id, "channel": channel.as_str(), "reason": "client unsubscribed"}),
        ));
        publish_status(&state, None);
        if state.subscribers.is_empty() {
            if immediate {
                if let Some(session) = state.close_port() {
                    self.serial_log
                        .finish_session(session, "client closed channel")
                        .await;
                }
            } else {
                drop(state);
                self.schedule_idle_close(channel).await;
            }
        }
        if let Some(channels) = self.peer_subscriptions.lock().await.get_mut(peer_id) {
            channels.remove(&channel);
        }
    }

    async fn disconnect_peer(&self, peer_id: &str) {
        let channels = self
            .peer_subscriptions
            .lock()
            .await
            .remove(peer_id)
            .unwrap_or_default();
        for channel in channels {
            let state_ref = self.channel(channel);
            let mut state = state_ref.lock().await;
            if state
                .owner
                .as_ref()
                .is_some_and(|(owner, _)| owner == peer_id)
            {
                self.finish_observer_redaction(&mut state);
                state.owner = None;
            }
            state.subscribers.remove(peer_id);
            publish_status(&state, None);
            let idle = state.subscribers.is_empty();
            drop(state);
            if idle {
                self.schedule_idle_close(channel).await;
            }
        }
    }

    async fn schedule_idle_close(&self, channel: Channel) {
        let state_ref = self.channel(channel);
        let mut state = state_ref.lock().await;
        state.cancel_idle_close();
        if !state.subscribers.is_empty() || !state.connected() {
            return;
        }
        let timeout = self.idle_timeout;
        let next_state = state_ref.clone();
        let serial_log = self.serial_log.clone();
        state.idle_task = Some(tokio::spawn(async move {
            tokio::time::sleep(timeout).await;
            let mut state = next_state.lock().await;
            if state.subscribers.is_empty() {
                debug!(
                    channel = state.channel.as_str(),
                    "closing idle serial channel"
                );
                if let Some(session) = state.close_port() {
                    serial_log.finish_session(session, "idle timeout").await;
                }
            }
            state.idle_task = None;
        }));
    }

    async fn read_serial(
        &self,
        channel: Channel,
        mut reader: ReadHalf<SerialStream>,
        log_session: Option<Uuid>,
    ) {
        let mut buffer = vec![0_u8; SERIAL_READ_BUFFER_BYTES];
        let mut pending = Vec::new();
        loop {
            let batch = read_serial_batch(
                &mut reader,
                &mut buffer,
                SERIAL_FORWARD_FLUSH_INTERVAL,
                SERIAL_FORWARD_MAX_BYTES,
            )
            .await;
            if !batch.bytes.is_empty() {
                if let Some(session) = log_session {
                    self.serial_log.record_rx(session, batch.bytes.clone());
                }
            }
            pending.extend_from_slice(&batch.bytes);
            let (text, consumed) = decode_utf8_stream(&pending);
            if consumed > 0 {
                pending.drain(..consumed);
            }
            if !text.is_empty() {
                self.broadcast_data(channel, text, consumed).await;
            }
            if let Some(error) = batch.error {
                self.broadcast_error(channel, "serial_io_error", &error.to_string())
                    .await;
                break;
            }
            if batch.eof {
                break;
            }
        }
        if !pending.is_empty() {
            let text = String::from_utf8_lossy(&pending).into_owned();
            self.broadcast_data(channel, text, pending.len()).await;
        }
        let state_ref = self.channel(channel);
        let mut state = state_ref.lock().await;
        if let Some(session) = state.log_session.take() {
            self.serial_log
                .finish_session(session, "serial port closed")
                .await;
        }
        self.finish_observer_redaction(&mut state);
        state.writer = None;
        state.path = None;
        state.baud = None;
        state.owner = None;
        state.reader_task = None;
        let closed = frame(
            "closed",
            json!({"channel": channel.as_str(), "reason": "serial port closed"}),
        );
        for sender in state.subscribers.values() {
            let _ = sender.send(closed.clone());
        }
        publish_status(&state, None);
    }

    async fn broadcast_data(&self, channel: Channel, text: String, byte_count: usize) {
        let state_ref = self.channel(channel);
        let mut state = state_ref.lock().await;
        state.sequence = state.sequence.wrapping_add(1);
        let host_t_mono_us = self.started.elapsed().as_micros().min(u64::MAX as u128) as u64;
        let sequence = state.sequence;
        if let Some(redaction) = state.observer_redaction.as_mut() {
            let owner = redaction.owner.clone();
            let visible = redaction.push(&text);
            if let Some(sender) = state.subscribers.get(&owner) {
                let _ = sender.send(data_frame(
                    channel,
                    sequence,
                    host_t_mono_us,
                    &text,
                    byte_count,
                ));
            }
            if !visible.is_empty() {
                let data = data_frame(channel, sequence, host_t_mono_us, &visible, visible.len());
                for (peer_id, sender) in &state.subscribers {
                    if peer_id != &owner {
                        let _ = sender.send(data.clone());
                    }
                }
            }
        } else {
            let data = data_frame(channel, sequence, host_t_mono_us, &text, byte_count);
            for sender in state.subscribers.values() {
                let _ = sender.send(data.clone());
            }
        }
    }

    fn finish_observer_redaction(&self, state: &mut ChannelState) {
        let Some(redaction) = state.observer_redaction.take() else {
            return;
        };
        let owner = redaction.owner.clone();
        let visible = redaction.finish();
        if visible.is_empty() {
            return;
        }
        state.sequence = state.sequence.wrapping_add(1);
        let host_t_mono_us = self.started.elapsed().as_micros().min(u64::MAX as u128) as u64;
        let data = data_frame(
            state.channel,
            state.sequence,
            host_t_mono_us,
            &visible,
            visible.len(),
        );
        for (peer_id, sender) in &state.subscribers {
            if peer_id != &owner {
                let _ = sender.send(data.clone());
            }
        }
    }

    async fn broadcast_error(&self, channel: Channel, code: &str, message: &str) {
        let state_ref = self.channel(channel);
        let state = state_ref.lock().await;
        for sender in state.subscribers.values() {
            send_error(sender, code, message, None, Some(channel), true);
        }
    }

    fn channel(&self, channel: Channel) -> Arc<Mutex<ChannelState>> {
        self.channels
            .get(&channel)
            .expect("all channels are initialized")
            .clone()
    }

    pub async fn shutdown(&self) {
        for state_ref in self.channels.values() {
            let mut state = state_ref.lock().await;
            state.cancel_idle_close();
            if let Some(session) = state.close_port() {
                self.serial_log
                    .finish_session(session, "host shutdown")
                    .await;
            }
        }
        self.serial_log.shutdown().await;
    }
}

#[derive(Debug)]
struct ClientRequest {
    kind: String,
    request_id: Option<String>,
    channel: Channel,
    baud: Option<u32>,
    owner: Option<String>,
    data: Option<Vec<u8>>,
    observer_redaction_token: Option<String>,
}

#[derive(Debug, thiserror::Error)]
#[error("{message}")]
struct RequestError {
    code: String,
    message: String,
    request_id: Option<String>,
}

fn request_error(
    code: &str,
    message: impl Into<String>,
    request_id: Option<String>,
) -> RequestError {
    RequestError {
        code: code.to_owned(),
        message: message.into(),
        request_id,
    }
}

fn parse_request(raw: &str) -> Result<ClientRequest, RequestError> {
    let value: Value = serde_json::from_str(raw)
        .map_err(|_| request_error("invalid_json", "broker request must be valid JSON", None))?;
    let object = value.as_object().ok_or_else(|| {
        request_error(
            "invalid_request",
            "broker request must be a JSON object",
            None,
        )
    })?;
    let request_id = object
        .get("request_id")
        .and_then(Value::as_str)
        .filter(|value| !value.is_empty())
        .map(|value| value.chars().take(128).collect());
    if object.get("protocol").and_then(Value::as_str) != Some(PROTOCOL) {
        return Err(request_error(
            "unsupported_protocol",
            format!("expected broker protocol {PROTOCOL}"),
            request_id,
        ));
    }
    let channel =
        Channel::parse(object.get("channel").and_then(Value::as_str)).ok_or_else(|| {
            request_error(
                "invalid_channel",
                "channel must be uart0 or uart1",
                request_id.clone(),
            )
        })?;
    let kind = object
        .get("type")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let mut request = ClientRequest {
        kind: kind.to_owned(),
        request_id: request_id.clone(),
        channel,
        baud: None,
        owner: None,
        data: None,
        observer_redaction_token: None,
    };
    match kind {
        "open" => {
            let baud = object
                .get("baud")
                .and_then(Value::as_u64)
                .unwrap_or(115_200);
            if !(MIN_BAUD..=MAX_BAUD).contains(&baud) {
                return Err(request_error(
                    "invalid_request",
                    "baud must be an integer between 300 and 4000000",
                    request_id,
                ));
            }
            request.baud = Some(baud as u32);
        }
        "write" => {
            let data = object.get("data").and_then(Value::as_object);
            let encoding = data
                .and_then(|value| value.get("encoding"))
                .and_then(Value::as_str)
                .unwrap_or("utf8");
            let text = data
                .and_then(|value| value.get("value"))
                .and_then(Value::as_str)
                .or_else(|| object.get("text").and_then(Value::as_str));
            let Some(text) = text else {
                return Err(request_error(
                    "invalid_write",
                    "write data must contain utf8 or base64 text",
                    request_id,
                ));
            };
            request.data = Some(match encoding {
                "utf8" => text.as_bytes().to_vec(),
                "base64" => BASE64
                    .decode(text.split_whitespace().collect::<String>())
                    .map_err(|_| {
                        request_error(
                            "invalid_write",
                            "write data is not valid base64",
                            request_id.clone(),
                        )
                    })?,
                _ => {
                    return Err(request_error(
                        "invalid_write",
                        "write data must contain utf8 or base64 text",
                        request_id,
                    ))
                }
            });
            if let Some(redaction) = object.get("observer_redaction") {
                let token = redaction
                    .as_object()
                    .and_then(|value| value.get("line_token"))
                    .and_then(Value::as_str)
                    .filter(|value| is_valid_redaction_token(value))
                    .ok_or_else(|| {
                        request_error(
                            "invalid_write",
                            "observer_redaction.line_token must be 8-64 ASCII letters or digits",
                            request_id.clone(),
                        )
                    })?;
                if encoding != "utf8" {
                    return Err(request_error(
                        "invalid_write",
                        "observer redaction is only supported for UTF-8 writes",
                        request_id,
                    ));
                }
                request.observer_redaction_token = Some(token.to_owned());
            }
        }
        "claim" => {
            request.owner = Some(
                object
                    .get("owner")
                    .and_then(Value::as_str)
                    .filter(|value| !value.trim().is_empty())
                    .unwrap_or("automation")
                    .trim()
                    .chars()
                    .take(80)
                    .collect(),
            );
        }
        "status" | "close" | "release" => {}
        _ => {
            return Err(request_error(
                "unsupported_request",
                format!("unsupported broker request: {kind}"),
                request_id,
            ))
        }
    }
    Ok(request)
}

async fn find_serial_port(channel: Channel) -> Result<Option<SerialPortInfo>, String> {
    tokio::task::spawn_blocking(move || {
        let mut ports = tokio_serial::available_ports().map_err(|error| error.to_string())?;
        ports.retain(|port| matches!(&port.port_type, SerialPortType::UsbPort(info) if info.vid == CH347_VENDOR_ID));
        ports.sort_by(|left, right| left.port_name.cmp(&right.port_name));
        let preferred = ports.iter().find(|port| port.port_name.to_ascii_uppercase().ends_with(channel.preferred_suffix())).cloned();
        Ok(preferred.or_else(|| ports.get(channel.fallback_index()).cloned()))
    })
    .await
    .map_err(|error| error.to_string())?
}

fn decode_utf8_stream(bytes: &[u8]) -> (String, usize) {
    let mut output = String::new();
    let mut offset = 0;
    while offset < bytes.len() {
        match std::str::from_utf8(&bytes[offset..]) {
            Ok(text) => {
                output.push_str(text);
                offset = bytes.len();
            }
            Err(error) => {
                let valid = error.valid_up_to();
                if valid > 0 {
                    // SAFETY: valid_up_to is guaranteed to end on a UTF-8 boundary.
                    output.push_str(unsafe {
                        std::str::from_utf8_unchecked(&bytes[offset..offset + valid])
                    });
                    offset += valid;
                }
                if let Some(length) = error.error_len() {
                    output.push('\u{fffd}');
                    offset += length;
                } else {
                    break;
                }
            }
        }
    }
    (output, offset)
}

fn is_valid_redaction_token(value: &str) -> bool {
    (8..=64).contains(&value.len()) && value.bytes().all(|byte| byte.is_ascii_alphanumeric())
}

fn data_frame(
    channel: Channel,
    sequence: u64,
    host_t_mono_us: u64,
    text: &str,
    byte_count: usize,
) -> Message {
    frame(
        "data",
        json!({
            "channel": channel.as_str(),
            "sequence": sequence,
            "host_t_mono_us": host_t_mono_us,
            "text": text,
            "byte_count": byte_count
        }),
    )
}

fn frame(kind: &str, fields: Value) -> Message {
    let mut object = serde_json::Map::from_iter([
        ("protocol".to_owned(), Value::String(PROTOCOL.to_owned())),
        ("type".to_owned(), Value::String(kind.to_owned())),
    ]);
    if let Value::Object(fields) = fields {
        object.extend(fields);
    }
    Message::Text(Value::Object(object).to_string().into())
}

fn status_frame(state: &ChannelState, request_id: Option<&str>) -> Message {
    frame(
        "status",
        json!({
            "request_id": request_id,
            "channel": state.channel.as_str(),
            "connected": state.connected(),
            "opening": false,
            "path": state.path,
            "baud": state.baud,
            "subscribers": state.subscribers.len(),
            "owner": state.owner.as_ref().map(|(client_id, label)| json!({"client_id": client_id, "label": label}))
        }),
    )
}

fn publish_status(state: &ChannelState, request_id: Option<&str>) {
    let frame = status_frame(state, request_id);
    for sender in state.subscribers.values() {
        let _ = sender.send(frame.clone());
    }
}

fn send_error(
    sender: &PeerSender,
    code: &str,
    message: &str,
    request_id: Option<&str>,
    channel: Option<Channel>,
    retryable: bool,
) {
    let _ = sender.send(frame(
        "error",
        json!({
            "request_id": request_id,
            "channel": channel.map(Channel::as_str),
            "code": code,
            "message": message,
            "retryable": retryable
        }),
    ));
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::duplex;

    #[tokio::test]
    async fn coalesces_small_serial_reads_within_the_flush_window() {
        let (mut writer, mut reader) = duplex(128);
        let writer_task = tokio::spawn(async move {
            writer.write_all(b"first").await.unwrap();
            tokio::time::sleep(Duration::from_millis(1)).await;
            writer.write_all(b"-second").await.unwrap();
        });
        let mut scratch = [0_u8; 8];

        let batch =
            read_serial_batch(&mut reader, &mut scratch, Duration::from_millis(50), 128).await;
        writer_task.await.unwrap();

        assert_eq!(batch.bytes, b"first-second");
        assert!(batch.eof);
        assert!(batch.error.is_none());
    }

    #[tokio::test]
    async fn caps_serial_forward_batches_without_losing_remaining_bytes() {
        let (mut writer, mut reader) = duplex(128);
        writer.write_all(b"abcdefghijkl").await.unwrap();
        let mut scratch = [0_u8; 16];

        let first =
            read_serial_batch(&mut reader, &mut scratch, Duration::from_millis(50), 5).await;
        let second =
            read_serial_batch(&mut reader, &mut scratch, Duration::from_millis(1), 16).await;

        assert_eq!(first.bytes, b"abcde");
        assert_eq!(second.bytes, b"fghijkl");
        assert!(!first.eof);
        assert!(!second.eof);
        assert!(first.error.is_none());
        assert!(second.error.is_none());
    }

    #[test]
    fn parses_versioned_open_and_rejects_unversioned_frames() {
        let request = parse_request(r#"{"protocol":"linkr-serial-broker.v1","type":"open","channel":"uart1","baud":1500000}"#).unwrap();
        assert_eq!(request.channel, Channel::Uart1);
        assert_eq!(request.baud, Some(1_500_000));
        let error = parse_request(r#"{"type":"open","channel":"uart0"}"#).unwrap_err();
        assert_eq!(error.code, "unsupported_protocol");
    }

    #[test]
    fn preserves_split_utf8_until_the_character_is_complete() {
        let bytes = "A，B".as_bytes();
        let (first, consumed) = decode_utf8_stream(&bytes[..3]);
        assert_eq!(first, "A");
        assert_eq!(consumed, 1);
        let mut pending = bytes[consumed..3].to_vec();
        pending.extend_from_slice(&bytes[3..]);
        let (second, consumed) = decode_utf8_stream(&pending);
        assert_eq!(second, "，B");
        assert_eq!(consumed, 4);
    }

    #[test]
    fn validates_write_encodings() {
        let utf8 = parse_request(r#"{"protocol":"linkr-serial-broker.v1","type":"write","channel":"uart0","data":{"encoding":"utf8","value":"完成"}}"#).unwrap();
        assert_eq!(utf8.data.unwrap(), "完成".as_bytes());
        let binary = parse_request(r#"{"protocol":"linkr-serial-broker.v1","type":"write","channel":"uart0","data":{"encoding":"base64","value":"AAE="}}"#).unwrap();
        assert_eq!(binary.data.unwrap(), vec![0, 1]);
    }

    #[test]
    fn validates_observer_redaction_tokens() {
        let request = parse_request(
            r#"{"protocol":"linkr-serial-broker.v1","type":"write","channel":"uart0","data":{"encoding":"utf8","value":"uname -a\r"},"observer_redaction":{"line_token":"eca90be0b06e496c9a410d875af47976"}}"#,
        )
        .unwrap();
        assert_eq!(
            request.observer_redaction_token.as_deref(),
            Some("eca90be0b06e496c9a410d875af47976")
        );

        let error = parse_request(
            r#"{"protocol":"linkr-serial-broker.v1","type":"write","channel":"uart0","data":{"encoding":"utf8","value":"x"},"observer_redaction":{"line_token":"bad token"}}"#,
        )
        .unwrap_err();
        assert_eq!(error.code, "invalid_write");
    }

    #[test]
    fn redacts_split_shell_bookkeeping_only_for_observers() {
        let token = "eca90be0b06e496c9a410d875af47976";
        let mut redaction = ObserverRedaction::new("mcp".to_owned(), token.to_owned());
        let mut visible =
            redaction.push("root@target:~$ uname -a\r\nroot@target:~$ __linkr_rc_eca90");
        visible.push_str(&redaction.push(
            "be0b06e496c9a410d875af47976=$?\r\nroot@target:~$ printf '__LINKR_RC_eca90be0b06e496c9a410d875af47976__%d\\n' \"$__linkr_rc_eca90be0b06e496c9a410d875af47976\"\r\nLinux target\r\n__LINKR_RC_eca90be0b06e496c9a410d875af47976__0\r\nroot@target:~$ ",
        ));
        visible.push_str(&redaction.finish());

        assert_eq!(
            visible,
            "root@target:~$ uname -a\r\nLinux target\r\nroot@target:~$ "
        );
        assert!(!visible.contains("LINKR_RC"));
        assert!(!visible.contains("linkr_rc"));
    }

    #[test]
    fn preserves_non_newline_output_before_the_exit_marker() {
        let token = "eca90be0b06e496c9a410d875af47976";
        assert_eq!(
            redact_internal_shell_text(
                "partial output__LINKR_RC_eca90be0b06e496c9a410d875af47976__0\r\n",
                token,
            ),
            "partial output\r\n"
        );
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn serial_stream_reports_os_exclusive_state() {
        let (serial, _peer) = SerialStream::pair().unwrap();
        assert!(serial.exclusive());
    }
}
