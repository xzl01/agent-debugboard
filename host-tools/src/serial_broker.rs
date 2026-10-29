use std::{
    collections::HashMap,
    sync::Arc,
    time::{Duration, Instant},
};

use axum::extract::ws::{Message, WebSocket};
use base64::{engine::general_purpose::STANDARD as BASE64, Engine};
use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use tokio::{
    io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt, ReadHalf, WriteHalf},
    sync::{mpsc, Mutex, OwnedSemaphorePermit, Semaphore},
    task::AbortHandle,
};
use tokio_serial::{SerialPortBuilderExt, SerialStream};
use tokio_util::sync::CancellationToken;
use tracing::{debug, warn};
use uuid::Uuid;

use crate::{
    lifecycle::{Lifecycle, SHUTDOWN_GRACE},
    serial_discovery,
    serial_io::SerialIo,
    serial_log::SerialLogService,
};

pub const PROTOCOL: &str = "linkr-serial-broker.v1";
const MIN_BAUD: u64 = 300;
const MAX_BAUD: u64 = 4_000_000;
const MAX_REDACTION_BUFFER_BYTES: usize = 65_536;
const REDACTION_SUFFIX_BYTES: usize = 512;
const SERIAL_READ_BUFFER_BYTES: usize = 16 * 1024;
const SERIAL_FORWARD_MAX_BYTES: usize = 16 * 1024;
const SERIAL_FORWARD_FLUSH_INTERVAL: Duration = Duration::from_millis(2);
const PEER_QUEUE_MESSAGES: usize = 64;
const PEER_QUEUE_BYTES: usize = 256 * 1024;
const PEER_COMMAND_MESSAGES: usize = 16;
const PEER_COMMAND_BYTES: usize = 256 * 1024;
const SERIAL_WRITE_TIMEOUT: Duration = Duration::from_secs(5);
pub(crate) const MAX_REQUEST_BYTES: usize = 64 * 1024;

type PeerId = String;
type SerialWriter = Arc<Mutex<WriteHalf<SerialIo<SerialStream>>>>;

struct QueuedFrame {
    frame: Message,
    // Account for the in-flight send too, not only frames still in the queue.
    _bytes: OwnedSemaphorePermit,
}

#[derive(Clone)]
struct PeerSender {
    sender: mpsc::Sender<QueuedFrame>,
    bytes: Arc<Semaphore>,
    cancel: CancellationToken,
}

impl PeerSender {
    fn new(cancel: CancellationToken) -> (Self, mpsc::Receiver<QueuedFrame>) {
        let (sender, receiver) = mpsc::channel(PEER_QUEUE_MESSAGES);
        (
            Self {
                sender,
                bytes: Arc::new(Semaphore::new(PEER_QUEUE_BYTES)),
                cancel,
            },
            receiver,
        )
    }

    fn send(&self, frame: Message) -> Result<(), ()> {
        if self.cancel.is_cancelled() {
            return Err(());
        }
        let size = match &frame {
            Message::Text(text) => text.len(),
            Message::Binary(bytes) | Message::Ping(bytes) | Message::Pong(bytes) => bytes.len(),
            Message::Close(frame) => frame.as_ref().map_or(0, |frame| 2 + frame.reason.len()),
        }
        .max(1);
        let permit = u32::try_from(size)
            .ok()
            .and_then(|size| self.bytes.clone().try_acquire_many_owned(size).ok());
        if let Some(permit) = permit {
            if self
                .sender
                .try_send(QueuedFrame {
                    frame,
                    _bytes: permit,
                })
                .is_ok()
            {
                return Ok(());
            }
        }
        // Never await a socket, cleanup, or another task under a channel lock.
        self.cancel.cancel();
        Err(())
    }
}

async fn write_serial<W: AsyncWrite + Unpin>(
    writer: &mut W,
    bytes: &[u8],
    cancel: &CancellationToken,
) -> std::io::Result<()> {
    tokio::select! {
        biased;
        _ = cancel.cancelled() => Err(std::io::Error::new(std::io::ErrorKind::Interrupted, "host shutting down")),
        result = tokio::time::timeout(SERIAL_WRITE_TIMEOUT, async {
            writer.write_all(bytes).await?;
            writer.flush().await
        }) => result.unwrap_or_else(|_| Err(std::io::Error::new(std::io::ErrorKind::TimedOut, "serial write/flush deadline exceeded"))),
    }
}

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

    fn interface(self) -> u8 {
        match self {
            Self::Uart0 => 0,
            Self::Uart1 => 2,
        }
    }
}

struct ChannelState {
    channel: Channel,
    path: Option<String>,
    baud: Option<u32>,
    writer: Option<SerialWriter>,
    reader_task: Option<AbortHandle>,
    idle_task: Option<AbortHandle>,
    device_serial: Option<String>,
    generation: u64,
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
            device_serial: None,
            generation: 0,
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
        self.generation = self.generation.wrapping_add(1);
        self.writer = None;
        self.device_serial = None;
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
    pub(crate) lifecycle: Lifecycle,
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
            lifecycle: Lifecycle::default(),
            idle_timeout,
            started: Instant::now(),
            serial_log,
        }
    }

    pub async fn serve_socket(&self, socket: WebSocket) {
        if self.lifecycle.cancel.is_cancelled() {
            return;
        }
        let peer_id = format!("host-{}", Uuid::new_v4());
        let (mut ws_sink, mut ws_stream) = socket.split();
        let cancel = self.lifecycle.cancel.child_token();
        let (tx, mut rx) = PeerSender::new(cancel.clone());
        // Keep reading EOF/Close even while a command is blocked on serial I/O.
        // Never await queue capacity: a flooding peer is canceled instead.
        let (commands, mut command_rx) = mpsc::channel::<QueuedFrame>(PEER_COMMAND_MESSAGES);
        let command_bytes = Arc::new(Semaphore::new(PEER_COMMAND_BYTES));
        let writer = async move {
            while let Some(queued) = rx.recv().await {
                if ws_sink.send(queued.frame).await.is_err() {
                    break;
                }
            }
        };

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

        let reader = async {
            while let Some(message) = ws_stream.next().await {
                let Ok(message) = message else { break };
                let size = match &message {
                    Message::Text(text) => text.len(),
                    Message::Binary(bytes) => bytes.len(),
                    Message::Close(_) => break,
                    Message::Ping(_) | Message::Pong(_) => continue,
                };
                let permit = u32::try_from(size.max(1))
                    .ok()
                    .filter(|_| size <= MAX_REQUEST_BYTES)
                    .and_then(|size| command_bytes.clone().try_acquire_many_owned(size).ok());
                let Some(permit) = permit else {
                    break;
                };
                if commands
                    .try_send(QueuedFrame {
                        frame: message,
                        _bytes: permit,
                    })
                    .is_err()
                {
                    break;
                }
            }
        };
        let executor = async {
            while let Some(queued) = command_rx.recv().await {
                let message = queued.frame;
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
        };
        tokio::select! {
            biased;
            _ = cancel.cancelled() => {}
            _ = writer => {}
            _ = reader => {}
            _ = executor => {}
        }
        cancel.cancel();
        self.disconnect_peer(&peer_id).await;
    }

    async fn dispatch(&self, peer_id: &str, sender: &PeerSender, raw: &str) {
        if self.lifecycle.cancel.is_cancelled() || sender.cancel.is_cancelled() {
            return;
        }
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
        if self.lifecycle.cancel.is_cancelled() || sender.cancel.is_cancelled() {
            return;
        }

        if state.connected()
            && request
                .device_serial
                .as_ref()
                .is_some_and(|serial| Some(serial) != state.device_serial.as_ref())
        {
            send_error(
                sender,
                "serial_device_conflict",
                "channel is open on another device",
                request.request_id.as_deref(),
                Some(channel),
                false,
            );
            return;
        }

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
            let candidate = match find_serial_port(channel, request.device_serial.clone()).await {
                Ok(candidate) => candidate,
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
            if self.lifecycle.cancel.is_cancelled() || sender.cancel.is_cancelled() {
                return;
            }
            let port = candidate.port;
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
            self.attach_port(
                &mut state,
                serial,
                port.port_name,
                candidate.identity.serial,
                baud,
            );
        }

        // No await between successful subscription and canceling the old idle timer.
        state.cancel_idle_close();
        state.subscribers.insert(peer_id.to_owned(), sender.clone());
        let _ = sender.send(frame(
            "opened",
            json!({
                "request_id": request.request_id,
                "channel": channel.as_str(),
                "path": state.path,
                "device_serial": state.device_serial,
                "baud": state.baud,
                "shared": true
            }),
        ));
        publish_status(&state, None);
    }

    fn attach_port(
        &self,
        state: &mut ChannelState,
        serial: SerialStream,
        path: String,
        device_serial: String,
        baud: u32,
    ) {
        let (reader, writer) = tokio::io::split(SerialIo::new(serial));
        state.path = Some(path.clone());
        state.baud = Some(baud);
        state.writer = Some(Arc::new(Mutex::new(writer)));
        state.device_serial = Some(device_serial);
        state.generation = state.generation.wrapping_add(1);
        state.log_session = self
            .serial_log
            .start_session(state.channel.as_str(), &path, baud);
        let broker = self.clone();
        let log_session = state.log_session;
        let generation = state.generation;
        let channel = state.channel;
        state.reader_task = self.lifecycle.spawn(async move {
            tokio::select! {
                biased;
                _ = broker.lifecycle.cancel.cancelled() => {}
                _ = broker.read_serial(channel, reader, log_session, generation) => {}
            }
        });
    }

    async fn write(&self, peer_id: &str, sender: &PeerSender, request: ClientRequest) {
        let channel = request.channel;
        let state_ref = self.channel(channel);
        let writer = state_ref.lock().await.writer.clone();
        let Some(writer) = writer else {
            send_error(
                sender,
                "serial_not_open",
                "channel is not open",
                request.request_id.as_deref(),
                Some(channel),
                true,
            );
            return;
        };
        // Serialize writes without holding the channel metadata lock across OS I/O.
        let mut output = writer.lock().await;
        let mut state = state_ref.lock().await;
        if !state.subscribers.contains_key(peer_id)
            || state
                .writer
                .as_ref()
                .is_none_or(|current| !Arc::ptr_eq(current, &writer))
            || self.lifecycle.cancel.is_cancelled()
            || sender.cancel.is_cancelled()
        {
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
            if state
                .owner
                .as_ref()
                .is_none_or(|(owner, _)| owner != peer_id)
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
        drop(state);
        let result = tokio::select! {
            biased;
            _ = sender.cancel.cancelled() => return,
            result = write_serial(&mut *output, &bytes, &self.lifecycle.cancel) => result,
        };
        if let Err(error) = result {
            send_error(
                sender,
                "serial_write_failed",
                &error.to_string(),
                request.request_id.as_deref(),
                Some(channel),
                false, // A prefix may already have reached the UART; never replay it.
            );
            return;
        }
        let _ = sender.send(frame(
            "write_ack",
            json!({"request_id": request.request_id, "channel": channel.as_str(), "bytes": bytes.len(), "completion": "driver_queue_empty"}),
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
                state.cancel_idle_close();
                let session = state.close_port();
                drop(state);
                self.finish_log(session, "client closed channel").await;
            } else {
                self.schedule_idle_close(&mut state, state_ref.clone());
            }
        }
    }

    async fn disconnect_peer(&self, peer_id: &str) {
        // Two channels are the authoritative subscription table. No reverse index
        // can be left inconsistent if an open/close future is canceled at an await.
        for state_ref in self.channels.values() {
            let mut state = state_ref.lock().await;
            if state.subscribers.remove(peer_id).is_none() {
                continue;
            }
            if state
                .owner
                .as_ref()
                .is_some_and(|(owner, _)| owner == peer_id)
            {
                self.finish_observer_redaction(&mut state);
                state.owner = None;
            }
            publish_status(&state, None);
            self.schedule_idle_close(&mut state, state_ref.clone());
        }
    }

    fn schedule_idle_close(&self, state: &mut ChannelState, next_state: Arc<Mutex<ChannelState>>) {
        if !state.subscribers.is_empty() || !state.connected() || state.idle_task.is_some() {
            return;
        }
        let timeout = self.idle_timeout;
        let broker = self.clone();
        state.idle_task = self.lifecycle.spawn(async move {
            tokio::select! {
                biased;
                _ = broker.lifecycle.cancel.cancelled() => return,
                _ = tokio::time::sleep(timeout) => {}
            }
            let mut state = next_state.lock().await;
            let session = if state.subscribers.is_empty() {
                debug!(
                    channel = state.channel.as_str(),
                    "closing idle serial channel"
                );
                state.close_port()
            } else {
                None
            };
            state.idle_task = None;
            drop(state);
            broker.finish_log(session, "idle timeout").await;
        });
    }

    async fn read_serial(
        &self,
        channel: Channel,
        mut reader: ReadHalf<SerialIo<SerialStream>>,
        log_session: Option<Uuid>,
        generation: u64,
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
                self.broadcast_data(channel, text, consumed, generation)
                    .await;
            }
            if let Some(error) = batch.error {
                self.broadcast_error(channel, "serial_io_error", &error.to_string(), generation)
                    .await;
                break;
            }
            if batch.eof {
                break;
            }
        }
        if !pending.is_empty() {
            let text = String::from_utf8_lossy(&pending).into_owned();
            self.broadcast_data(channel, text, pending.len(), generation)
                .await;
        }
        let state_ref = self.channel(channel);
        let mut state = state_ref.lock().await;
        if state.generation != generation {
            return;
        }
        let session = state.log_session.take();
        self.finish_observer_redaction(&mut state);
        state.writer = None;
        state.path = None;
        state.baud = None;
        state.owner = None;
        state.reader_task = None;
        state.device_serial = None;
        let closed = frame(
            "closed",
            json!({"channel": channel.as_str(), "reason": "serial port closed"}),
        );
        for sender in state.subscribers.values() {
            let _ = sender.send(closed.clone());
        }
        publish_status(&state, None);
        drop(state);
        self.finish_log(session, "serial port closed").await;
    }

    async fn broadcast_data(
        &self,
        channel: Channel,
        text: String,
        byte_count: usize,
        generation: u64,
    ) {
        let state_ref = self.channel(channel);
        let mut state = state_ref.lock().await;
        if state.generation != generation {
            return;
        }
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

    async fn broadcast_error(&self, channel: Channel, code: &str, message: &str, generation: u64) {
        let state_ref = self.channel(channel);
        let state = state_ref.lock().await;
        if state.generation != generation {
            return;
        }
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
        self.lifecycle.shutdown().await;
        for state_ref in self.channels.values() {
            let mut state = state_ref.lock().await;
            state.cancel_idle_close();
            state.subscribers.clear();
            let session = state.close_port();
            drop(state);
            self.finish_log(session, "host shutdown").await;
        }
        if tokio::time::timeout(SHUTDOWN_GRACE, self.serial_log.shutdown())
            .await
            .is_err()
        {
            warn!("serial log shutdown deadline exceeded; archive completion is not guaranteed");
        }
    }

    async fn finish_log(&self, session: Option<Uuid>, reason: &str) {
        if let Some(session) = session {
            if tokio::time::timeout(
                SHUTDOWN_GRACE,
                self.serial_log.finish_session(session, reason),
            )
            .await
            .is_err()
            {
                warn!(%session, "serial log finish deadline exceeded");
            }
        }
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
    device_serial: Option<String>,
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
        device_serial: None,
    };
    match kind {
        "open" => {
            if let Some(serial) = object.get("device_serial") {
                let serial = serial
                    .as_str()
                    .filter(|serial| !serial.is_empty() && serial.len() <= 128)
                    .ok_or_else(|| {
                        request_error(
                            "invalid_request",
                            "device_serial must be a nonempty USB serial string (max 128 bytes)",
                            request_id.clone(),
                        )
                    })?;
                request.device_serial = Some(serial.to_owned());
            }
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

async fn find_serial_port(
    channel: Channel,
    serial: Option<String>,
) -> Result<serial_discovery::Candidate, String> {
    // Unit/loopback tests must never discover and open a real device, even when
    // testing a broken admission check. They inject Unix virtual ports instead.
    if cfg!(test) {
        return Err("physical serial discovery disabled in unit tests".to_owned());
    }
    tokio::task::spawn_blocking(move || {
        serial_discovery::discover(channel.interface(), serial.as_deref())
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
            "device_serial": state.device_serial,
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

    fn test_broker(idle_timeout: Duration) -> SerialBroker {
        SerialBroker::new(
            idle_timeout,
            SerialLogService::disabled("unused-test-archive".into()),
        )
    }

    #[cfg(unix)]
    fn request(kind: &str, baud: u32) -> ClientRequest {
        parse_request(
            &json!({"protocol": PROTOCOL, "type": kind, "channel": "uart0", "baud": baud})
                .to_string(),
        )
        .unwrap()
    }

    #[cfg(unix)]
    async fn attach_virtual_port(broker: &SerialBroker) -> SerialStream {
        let (serial, peer) = SerialStream::pair().unwrap();
        let state_ref = broker.channel(Channel::Uart0);
        let mut state = state_ref.lock().await;
        broker.attach_port(
            &mut state,
            serial,
            "virtual-test-only".into(),
            "test-device".into(),
            115200,
        );
        peer
    }

    #[tokio::test]
    async fn peer_queue_bounds_messages_and_inflight_bytes() {
        let (sender, _rx) = PeerSender::new(CancellationToken::new());
        for _ in 0..PEER_QUEUE_MESSAGES {
            sender.send(Message::Text("x".into())).unwrap();
        }
        assert!(sender.send(Message::Text("x".into())).is_err());
        assert!(sender.cancel.is_cancelled());

        let (sender, mut rx) = PeerSender::new(CancellationToken::new());
        sender
            .send(Message::Text("x".repeat(PEER_QUEUE_BYTES).into()))
            .unwrap();
        let in_flight = rx.recv().await.unwrap();
        assert_eq!(sender.bytes.available_permits(), 0);
        assert!(sender.send(Message::Text("x".into())).is_err());
        drop(in_flight);
        assert_eq!(sender.bytes.available_permits(), PEER_QUEUE_BYTES);
    }

    #[tokio::test]
    async fn slow_peer_cancellation_does_not_block_broadcast_or_other_peer() {
        let broker = test_broker(Duration::from_secs(1));
        let (slow, _slow_rx) = PeerSender::new(broker.lifecycle.cancel.child_token());
        let (fast, mut fast_rx) = PeerSender::new(broker.lifecycle.cancel.child_token());
        let state_ref = broker.channel(Channel::Uart0);
        {
            let mut state = state_ref.lock().await;
            state.subscribers.insert("slow".into(), slow.clone());
            state.subscribers.insert("fast".into(), fast.clone());
        }
        for _ in 0..=PEER_QUEUE_MESSAGES {
            tokio::time::timeout(
                Duration::from_millis(100),
                broker.broadcast_data(Channel::Uart0, "rx".into(), 2, 0),
            )
            .await
            .unwrap();
            assert!(fast_rx.recv().await.is_some());
        }
        assert!(slow.cancel.is_cancelled());
        assert!(!fast.cancel.is_cancelled());
        assert!(!broker.lifecycle.cancel.is_cancelled());
        broker.disconnect_peer("slow").await;
        assert_eq!(state_ref.lock().await.subscribers.len(), 1);
        broker.shutdown().await;
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn overflowing_live_socket_automatically_releases_subscription_and_claim() {
        use axum::{extract::WebSocketUpgrade, routing::get, Router};
        use tokio_tungstenite::{connect_async, tungstenite::Message as WsMessage};
        let broker = test_broker(Duration::from_secs(30));
        let _serial_peer = attach_virtual_port(&broker).await;
        let handler_broker = broker.clone();
        let app = Router::new().route(
            "/serial",
            get(move |ws: WebSocketUpgrade| {
                let broker = handler_broker.clone();
                async move {
                    ws.on_upgrade(move |socket| async move {
                        let lifecycle = broker.lifecycle.clone();
                        lifecycle.spawn(async move {
                            broker.serve_socket(socket).await;
                        });
                    })
                }
            }),
        );
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let address = listener.local_addr().unwrap();
        let stopped = broker.lifecycle.cancel.clone();
        let server = tokio::spawn(async move {
            axum::serve(listener, app)
                .with_graceful_shutdown(stopped.cancelled_owned())
                .await
                .unwrap();
        });
        let (mut socket, _) = connect_async(format!("ws://{address}/serial"))
            .await
            .unwrap();
        let hello: Value =
            serde_json::from_str(socket.next().await.unwrap().unwrap().to_text().unwrap()).unwrap();
        let peer_id = hello["client_id"].as_str().unwrap();
        socket
            .send(WsMessage::Text(
                json!({"protocol": PROTOCOL, "type": "open", "channel": "uart0"})
                    .to_string()
                    .into(),
            ))
            .await
            .unwrap();
        assert!(socket
            .next()
            .await
            .unwrap()
            .unwrap()
            .to_text()
            .unwrap()
            .contains("opened"));
        let state_ref = broker.channel(Channel::Uart0);
        let (fast, mut fast_rx) = PeerSender::new(broker.lifecycle.cancel.child_token());
        {
            let mut state = state_ref.lock().await;
            let slow = state.subscribers.get(peer_id).unwrap().clone();
            state.owner = Some((peer_id.to_owned(), "slow".into()));
            state.subscribers.insert("fast".into(), fast.clone());
            // Fill without yielding: deterministic regardless of kernel socket buffers.
            for _ in 0..=PEER_QUEUE_MESSAGES {
                let _ = slow.send(Message::Text("queued".into()));
            }
            assert!(slow.cancel.is_cancelled());
            publish_status(&state, None);
        }
        assert!(fast_rx.recv().await.is_some());
        tokio::time::timeout(Duration::from_secs(1), async {
            loop {
                if !state_ref.lock().await.subscribers.contains_key(peer_id) {
                    break;
                }
                tokio::task::yield_now().await;
            }
        })
        .await
        .unwrap();
        let state = state_ref.lock().await;
        assert!(state.owner.is_none());
        assert!(state.subscribers.contains_key("fast"));
        assert!(state.connected());
        assert!(!fast.cancel.is_cancelled());
        drop(state);
        broker.shutdown().await;
        server.await.unwrap();
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn tcp_disconnect_cancels_backpressured_write_and_releases_claim() {
        use axum::{extract::WebSocketUpgrade, routing::get, Router};
        use tokio_tungstenite::{connect_async, tungstenite::Message as WsMessage};
        let broker = test_broker(Duration::from_secs(30));
        let mut serial_peer = attach_virtual_port(&broker).await;
        let handler_broker = broker.clone();
        let app = Router::new().route(
            "/serial",
            get(move |ws: WebSocketUpgrade| {
                let broker = handler_broker.clone();
                async move {
                    ws.on_upgrade(move |socket| async move {
                        let lifecycle = broker.lifecycle.clone();
                        lifecycle.spawn(async move {
                            broker.serve_socket(socket).await;
                        });
                    })
                }
            }),
        );
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let address = listener.local_addr().unwrap();
        let stopped = broker.lifecycle.cancel.clone();
        let server = tokio::spawn(async move {
            axum::serve(listener, app)
                .with_graceful_shutdown(stopped.cancelled_owned())
                .await
                .unwrap();
        });
        let (mut socket, _) = connect_async(format!("ws://{address}/serial"))
            .await
            .unwrap();
        let hello: Value =
            serde_json::from_str(socket.next().await.unwrap().unwrap().to_text().unwrap()).unwrap();
        let peer_id = hello["client_id"].as_str().unwrap();
        for (kind, expected) in [("open", "opened"), ("claim", "claimed")] {
            socket
                .send(WsMessage::Text(
                    json!({"protocol": PROTOCOL, "type": kind, "channel": "uart0"})
                        .to_string()
                        .into(),
                ))
                .await
                .unwrap();
            loop {
                let message = socket.next().await.unwrap().unwrap();
                let frame: Value = serde_json::from_str(message.to_text().unwrap()).unwrap();
                if frame["type"] == expected {
                    break;
                }
            }
        }
        socket.send(WsMessage::Text(json!({"protocol": PROTOCOL, "type": "write", "channel": "uart0", "text": "x".repeat(60 * 1024)}).to_string().into())).await.unwrap();
        let mut byte = [0];
        tokio::time::timeout(Duration::from_secs(1), serial_peer.read_exact(&mut byte))
            .await
            .unwrap()
            .unwrap();
        let state_ref = broker.channel(Channel::Uart0);
        let peer_cancel = state_ref.lock().await.subscribers[peer_id].cancel.clone();
        assert!(!peer_cancel.is_cancelled());
        // Do not read the rest of the PTY. Drop TCP without sending a WS Close.
        drop(socket);
        tokio::time::timeout(Duration::from_millis(350), async {
            loop {
                let state = state_ref.lock().await;
                if !state.subscribers.contains_key(peer_id) {
                    assert!(state.owner.is_none());
                    assert!(state.idle_task.is_some());
                    break;
                }
                drop(state);
                tokio::time::sleep(Duration::from_millis(5)).await;
            }
        })
        .await
        .unwrap();
        assert!(peer_cancel.is_cancelled());
        tokio::time::timeout(Duration::from_secs(1), broker.shutdown())
            .await
            .unwrap();
        server.await.unwrap();
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn failed_open_preserves_idle_timer_and_reclaims_port_reader_and_log() {
        let root = tempfile::tempdir().unwrap();
        let log = SerialLogService::new(crate::serial_log::SerialLogConfig {
            enabled: true,
            root: root.path().to_path_buf(),
            segment_bytes: 1024,
            total_bytes: 4096,
            retention: Duration::from_secs(60),
            queue_records: 16,
        })
        .await
        .unwrap();
        let broker = SerialBroker::new(Duration::from_millis(40), log.clone());
        let _peer = attach_virtual_port(&broker).await;
        let (sender, mut rx) = PeerSender::new(broker.lifecycle.cancel.child_token());
        broker.open("old", &sender, request("open", 115200)).await;
        broker.disconnect_peer("old").await;
        let state_ref = broker.channel(Channel::Uart0);
        let (idle_id, reader, session) = {
            let state = state_ref.lock().await;
            (
                state.idle_task.as_ref().unwrap().id(),
                state.reader_task.clone().unwrap(),
                state.log_session.unwrap(),
            )
        };
        broker.open("failed", &sender, request("open", 9600)).await;
        assert_eq!(
            state_ref.lock().await.idle_task.as_ref().unwrap().id(),
            idle_id
        );
        assert!(state_ref.lock().await.subscribers.is_empty());
        let mut saw_conflict = false;
        while let Ok(frame) = rx.try_recv() {
            if let Message::Text(text) = frame.frame {
                saw_conflict |= text.contains("baud_conflict");
            }
        }
        assert!(saw_conflict);
        tokio::time::timeout(Duration::from_secs(1), async {
            loop {
                if !state_ref.lock().await.connected() && reader.is_finished() {
                    break;
                }
                tokio::time::sleep(Duration::from_millis(5)).await;
            }
        })
        .await
        .unwrap();
        broker.shutdown().await;
        assert!(log
            .find(session)
            .unwrap()
            .unwrap()
            .manifest
            .ended_at
            .is_some());
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn shutdown_cancels_blocked_serial_write_and_rejects_later_open() {
        let broker = test_broker(Duration::from_secs(30));
        let mut peer = attach_virtual_port(&broker).await;
        let (sender, _rx) = PeerSender::new(broker.lifecycle.cancel.child_token());
        broker
            .open("writer", &sender, request("open", 115200))
            .await;
        let mut write = request("status", 115200);
        write.kind = "write".into();
        write.data = Some(vec![b'x'; MAX_REQUEST_BYTES]);
        let task_broker = broker.clone();
        let write_task = broker
            .lifecycle
            .spawn(async move {
                task_broker.write("writer", &sender, write).await;
            })
            .unwrap();
        let mut byte = [0];
        tokio::time::timeout(Duration::from_secs(1), peer.read_exact(&mut byte))
            .await
            .unwrap()
            .unwrap();
        assert!(
            !write_task.is_finished(),
            "virtual tty should backpressure the remaining payload"
        );
        tokio::time::timeout(Duration::from_millis(500), broker.shutdown())
            .await
            .unwrap();
        assert!(write_task.is_finished());
        let (late, mut rx) = PeerSender::new(CancellationToken::new());
        // If admission regresses, this would enumerate ports. Cancellation must win first.
        broker.open("late", &late, request("open", 115200)).await;
        assert!(rx.try_recv().is_err());
        let state_ref = broker.channel(Channel::Uart0);
        let state = state_ref.lock().await;
        assert!(!state.connected());
        assert!(state.subscribers.is_empty());
    }

    #[tokio::test]
    async fn cancellation_interrupts_serial_flush() {
        use std::{
            pin::Pin,
            sync::atomic::{AtomicUsize, Ordering},
            task::{Context, Poll},
        };
        struct StalledFlush {
            calls: Arc<AtomicUsize>,
            queued: Arc<AtomicUsize>,
        }
        impl crate::serial_io::OutputQueue for StalledFlush {
            fn bytes_pending(&self) -> std::io::Result<u32> {
                let queued = self.queued.load(Ordering::SeqCst);
                if queued == usize::MAX {
                    return Err(std::io::Error::new(
                        std::io::ErrorKind::Unsupported,
                        "queue query unsupported",
                    ));
                }
                Ok(queued as u32)
            }
        }
        impl AsyncWrite for StalledFlush {
            fn poll_write(
                self: Pin<&mut Self>,
                _: &mut Context<'_>,
                bytes: &[u8],
            ) -> Poll<std::io::Result<usize>> {
                Poll::Ready(Ok(bytes.len()))
            }
            fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<std::io::Result<()>> {
                // Model the real tokio-serial -> mio-serial -> tcdrain boundary,
                // not an asynchronously Pending flush. Calling this blocks the runtime.
                self.calls.fetch_add(1, Ordering::SeqCst);
                std::thread::sleep(Duration::from_millis(300));
                Poll::Ready(Ok(()))
            }
            fn poll_shutdown(
                self: Pin<&mut Self>,
                cx: &mut Context<'_>,
            ) -> Poll<std::io::Result<()>> {
                self.poll_flush(cx)
            }
        }
        let cancel = CancellationToken::new();
        let calls = Arc::new(AtomicUsize::new(0));
        let queued = Arc::new(AtomicUsize::new(1));
        let mut writer = SerialIo::new(StalledFlush {
            calls: calls.clone(),
            queued: queued.clone(),
        });
        let trigger = cancel.clone();
        let trigger_task = tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(20)).await;
            trigger.cancel();
        });
        let started = Instant::now();
        assert_eq!(
            tokio::time::timeout(
                Duration::from_millis(50),
                write_serial(&mut writer, b"x", &cancel)
            )
            .await
            .unwrap()
            .unwrap_err()
            .kind(),
            std::io::ErrorKind::Interrupted
        );
        trigger_task.await.unwrap();
        assert!(started.elapsed() < Duration::from_millis(150));
        assert_eq!(calls.load(Ordering::SeqCst), 0);
        // Completion requires a queue-empty report, but never calls tcdrain.
        queued.store(0, Ordering::SeqCst);
        write_serial(&mut writer, b"x", &CancellationToken::new())
            .await
            .unwrap();
        assert_eq!(calls.load(Ordering::SeqCst), 0);
        queued.store(usize::MAX, Ordering::SeqCst);
        assert_eq!(
            write_serial(&mut writer, b"x", &CancellationToken::new())
                .await
                .unwrap_err()
                .kind(),
            std::io::ErrorKind::Unsupported
        );
    }

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
