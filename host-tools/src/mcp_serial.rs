use std::{
    collections::{HashMap, HashSet, VecDeque},
    sync::Arc,
    time::{Duration, Instant},
};

use futures_util::{SinkExt, StreamExt};
use regex::{Regex, RegexBuilder};
use serde_json::{json, Value};
use tokio::sync::{mpsc, oneshot, Mutex, Notify};
use tokio_tungstenite::{connect_async, tungstenite::Message};
use url::Url;
use uuid::Uuid;

use crate::serial_broker::{redact_internal_shell_text, PROTOCOL};

const REQUEST_TIMEOUT: Duration = Duration::from_secs(10);
const MAX_HISTORY_UNITS: usize = 1_048_576;

#[derive(Debug, Clone)]
pub struct BrokerClientError {
    pub code: String,
    pub message: String,
    pub details: Value,
}

impl BrokerClientError {
    fn new(code: impl Into<String>, message: impl Into<String>) -> Self {
        Self {
            code: code.into(),
            message: message.into(),
            details: json!({}),
        }
    }

    fn with_details(mut self, details: Value) -> Self {
        self.details = details;
        self
    }
}

impl std::fmt::Display for BrokerClientError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(&self.message)
    }
}

impl std::error::Error for BrokerClientError {}

#[derive(Debug)]
struct PendingRequest {
    expected: HashSet<String>,
    response: oneshot::Sender<Result<Value, BrokerClientError>>,
}

#[derive(Debug, Default)]
struct History {
    open: bool,
    claimed: bool,
    status: Option<Value>,
    earliest_cursor: u64,
    latest_cursor: u64,
    units: VecDeque<u16>,
    notify: Arc<Notify>,
}

impl History {
    fn at_cursor(cursor: u64) -> Self {
        Self {
            earliest_cursor: cursor,
            latest_cursor: cursor,
            ..Self::default()
        }
    }

    fn append(&mut self, text: &str) {
        let encoded = text.encode_utf16().collect::<Vec<_>>();
        self.latest_cursor = self.latest_cursor.saturating_add(encoded.len() as u64);
        self.units.extend(encoded);
        while self.units.len() > MAX_HISTORY_UNITS {
            self.units.pop_front();
            self.earliest_cursor = self.earliest_cursor.saturating_add(1);
        }
        if self.units.is_empty() {
            self.earliest_cursor = self.latest_cursor;
        }
        self.notify.notify_waiters();
    }

    fn collect(
        &self,
        channel: &str,
        cursor: u64,
        max_units: usize,
    ) -> Result<Value, BrokerClientError> {
        if cursor < self.earliest_cursor {
            return Err(BrokerClientError::new(
                "serial_cursor_expired",
                format!(
                    "cursor {cursor} is older than retained cursor {}",
                    self.earliest_cursor
                ),
            )
            .with_details(json!({
                "earliest_cursor": self.earliest_cursor,
                "latest_cursor": self.latest_cursor
            })));
        }
        if cursor > self.latest_cursor {
            return Err(BrokerClientError::new(
                "serial_cursor_ahead",
                format!(
                    "cursor {cursor} is newer than current cursor {}; reconnect or restart from the reported cursor",
                    self.latest_cursor
                ),
            )
            .with_details(json!({
                "earliest_cursor": self.earliest_cursor,
                "latest_cursor": self.latest_cursor
            })));
        }
        let requested = cursor;
        let offset = (requested - self.earliest_cursor) as usize;
        let available = self.units.len().saturating_sub(offset);
        let take = available.min(max_units);
        let selected = self
            .units
            .iter()
            .skip(offset)
            .take(take)
            .copied()
            .collect::<Vec<_>>();
        Ok(json!({
            "channel": channel,
            "cursor": requested,
            "next_cursor": requested + take as u64,
            "earliest_cursor": self.earliest_cursor,
            "latest_cursor": self.latest_cursor,
            "truncated": take < available,
            "text": String::from_utf16_lossy(&selected)
        }))
    }
}

#[derive(Debug)]
struct ClientState {
    client_id: Option<String>,
    pending: HashMap<String, PendingRequest>,
    channels: HashMap<String, History>,
    closed: bool,
}

impl Default for ClientState {
    fn default() -> Self {
        Self::with_cursor_checkpoints(&HashMap::new())
    }
}

impl ClientState {
    fn with_cursor_checkpoints(checkpoints: &HashMap<String, u64>) -> Self {
        Self {
            client_id: None,
            pending: HashMap::new(),
            channels: HashMap::from([
                (
                    "uart0".to_owned(),
                    History::at_cursor(*checkpoints.get("uart0").unwrap_or(&0)),
                ),
                (
                    "uart1".to_owned(),
                    History::at_cursor(*checkpoints.get("uart1").unwrap_or(&0)),
                ),
            ]),
            closed: false,
        }
    }
}

#[derive(Clone)]
pub struct SerialBrokerClient {
    outgoing: mpsc::UnboundedSender<Message>,
    state: Arc<Mutex<ClientState>>,
}

impl SerialBrokerClient {
    pub async fn connect(url: Url) -> Result<Self, BrokerClientError> {
        Self::connect_with_cursor_checkpoints(url, &HashMap::new()).await
    }

    pub async fn connect_with_cursor_checkpoints(
        url: Url,
        checkpoints: &HashMap<String, u64>,
    ) -> Result<Self, BrokerClientError> {
        let (socket, _) = connect_async(url.as_str()).await.map_err(|error| {
            BrokerClientError::new(
                "broker_connect_failed",
                format!("cannot connect to Serial Broker at {url}: {error}"),
            )
            .with_details(json!({
                "dependency": "serial_broker",
                "endpoint": url.as_str(),
                "retry_after_ms": 1_000
            }))
        })?;
        let (mut sink, mut stream) = socket.split();
        let (outgoing, mut outgoing_rx) = mpsc::unbounded_channel::<Message>();
        let state = Arc::new(Mutex::new(ClientState::with_cursor_checkpoints(
            checkpoints,
        )));
        let (hello_tx, hello_rx) = oneshot::channel::<Result<String, BrokerClientError>>();

        tokio::spawn(async move {
            while let Some(message) = outgoing_rx.recv().await {
                if sink.send(message).await.is_err() {
                    break;
                }
            }
            let _ = sink.close().await;
        });

        let read_state = state.clone();
        tokio::spawn(async move {
            let mut hello_tx = Some(hello_tx);
            while let Some(message) = stream.next().await {
                let Ok(message) = message else { break };
                let text = match message {
                    Message::Text(text) => text.to_string(),
                    Message::Binary(bytes) => match String::from_utf8(bytes.to_vec()) {
                        Ok(text) => text,
                        Err(_) => continue,
                    },
                    Message::Close(_) => break,
                    Message::Ping(_) | Message::Pong(_) | Message::Frame(_) => continue,
                };
                let Ok(frame) = serde_json::from_str::<Value>(&text) else {
                    continue;
                };
                if frame.get("protocol").and_then(Value::as_str) != Some(PROTOCOL) {
                    continue;
                }

                let mut state = read_state.lock().await;
                if frame.get("type").and_then(Value::as_str) == Some("hello") {
                    let client_id = frame
                        .get("client_id")
                        .and_then(Value::as_str)
                        .unwrap_or("unknown")
                        .to_owned();
                    state.client_id = Some(client_id.clone());
                    if let Some(sender) = hello_tx.take() {
                        let _ = sender.send(Ok(client_id));
                    }
                }
                if let Some(channel) = broker_channel(frame.get("channel").and_then(Value::as_str))
                {
                    if let Some(history) = state.channels.get_mut(channel) {
                        match frame.get("type").and_then(Value::as_str) {
                            Some("data") => {
                                if let Some(text) = frame.get("text").and_then(Value::as_str) {
                                    history.append(text);
                                }
                            }
                            Some("status") => history.status = Some(frame.clone()),
                            Some("opened") => history.open = true,
                            Some("closed") => {
                                history.open = false;
                                history.claimed = false;
                                history.notify.notify_waiters();
                            }
                            Some("claimed") => history.claimed = true,
                            Some("released") => history.claimed = false,
                            _ => {}
                        }
                    }
                }

                let pending =
                    frame
                        .get("request_id")
                        .and_then(Value::as_str)
                        .and_then(|request_id| {
                            let expected = state.pending.get(request_id)?;
                            let kind = frame
                                .get("type")
                                .and_then(Value::as_str)
                                .unwrap_or_default();
                            if kind != "error" && !expected.expected.contains(kind) {
                                return None;
                            }
                            state.pending.remove(request_id)
                        });
                drop(state);
                if let Some(pending) = pending {
                    let result = if frame.get("type").and_then(Value::as_str) == Some("error") {
                        Err(BrokerClientError {
                            code: frame
                                .get("code")
                                .and_then(Value::as_str)
                                .unwrap_or("broker_error")
                                .to_owned(),
                            message: frame
                                .get("message")
                                .and_then(Value::as_str)
                                .unwrap_or("Serial Broker request failed")
                                .to_owned(),
                            details: frame.clone(),
                        })
                    } else {
                        Ok(frame)
                    };
                    let _ = pending.response.send(result);
                }
            }

            let error = BrokerClientError::new("broker_closed", "Serial Broker connection closed");
            let mut state = read_state.lock().await;
            state.closed = true;
            for history in state.channels.values_mut() {
                history.open = false;
                history.claimed = false;
                history.notify.notify_waiters();
            }
            for (_, pending) in state.pending.drain() {
                let _ = pending.response.send(Err(error.clone()));
            }
            if let Some(sender) = hello_tx.take() {
                let _ = sender.send(Err(error));
            }
        });

        match tokio::time::timeout(REQUEST_TIMEOUT, hello_rx).await {
            Ok(Ok(Ok(_))) => Ok(Self { outgoing, state }),
            Ok(Ok(Err(error))) => Err(error),
            Ok(Err(_)) => Err(BrokerClientError::new(
                "broker_closed",
                "Serial Broker closed before hello",
            )),
            Err(_) => Err(BrokerClientError::new(
                "broker_connect_timeout",
                "Serial Broker hello timed out",
            )),
        }
    }

    pub async fn is_closed(&self) -> bool {
        self.state.lock().await.closed
    }

    pub async fn cursor_checkpoints(&self) -> HashMap<String, u64> {
        self.state
            .lock()
            .await
            .channels
            .iter()
            .map(|(channel, history)| (channel.clone(), history.latest_cursor))
            .collect()
    }

    async fn request(
        &self,
        kind: &str,
        channel: &str,
        fields: Value,
        expected: &[&str],
        timeout: Duration,
    ) -> Result<Value, BrokerClientError> {
        let channel = normalize_channel(channel)?;
        let request_id = format!("mcp-{}", Uuid::new_v4());
        let mut frame = serde_json::Map::from_iter([
            ("protocol".to_owned(), Value::String(PROTOCOL.to_owned())),
            ("type".to_owned(), Value::String(kind.to_owned())),
            ("request_id".to_owned(), Value::String(request_id.clone())),
            ("channel".to_owned(), Value::String(channel.to_owned())),
        ]);
        if let Value::Object(fields) = fields {
            frame.extend(fields);
        }
        let (tx, rx) = oneshot::channel();
        {
            let mut state = self.state.lock().await;
            if state.closed {
                return Err(BrokerClientError::new(
                    "broker_closed",
                    "Serial Broker connection is closed",
                ));
            }
            state.pending.insert(
                request_id.clone(),
                PendingRequest {
                    expected: expected.iter().map(|value| (*value).to_owned()).collect(),
                    response: tx,
                },
            );
        }
        if self
            .outgoing
            .send(Message::Text(Value::Object(frame).to_string().into()))
            .is_err()
        {
            self.state.lock().await.pending.remove(&request_id);
            return Err(BrokerClientError::new(
                "broker_send_failed",
                "Serial Broker writer is closed",
            ));
        }
        match tokio::time::timeout(timeout, rx).await {
            Ok(Ok(result)) => result,
            Ok(Err(_)) => Err(BrokerClientError::new(
                "broker_closed",
                "Serial Broker request was cancelled",
            )),
            Err(_) => {
                self.state.lock().await.pending.remove(&request_id);
                Err(BrokerClientError::new(
                    "broker_request_timeout",
                    format!("{kind} request timed out"),
                )
                .with_details(json!({"request_id": request_id})))
            }
        }
    }

    pub async fn open(&self, channel: &str, baud: u32) -> Result<Value, BrokerClientError> {
        if !(300..=4_000_000).contains(&baud) {
            return Err(BrokerClientError::new(
                "invalid_baud",
                "baud must be between 300 and 4000000",
            ));
        }
        let channel = normalize_channel(channel)?;
        let mut response = self
            .request(
                "open",
                channel,
                json!({"baud": baud}),
                &["opened"],
                REQUEST_TIMEOUT,
            )
            .await?;
        response["cursor"] = Value::from(self.current_cursor(channel).await?);
        Ok(response)
    }

    pub async fn status(&self, channel: &str) -> Result<Value, BrokerClientError> {
        self.request("status", channel, json!({}), &["status"], REQUEST_TIMEOUT)
            .await
    }

    pub async fn claim(&self, channel: &str, owner: &str) -> Result<Value, BrokerClientError> {
        self.request(
            "claim",
            channel,
            json!({"owner": owner.chars().take(80).collect::<String>()}),
            &["claimed"],
            REQUEST_TIMEOUT,
        )
        .await
    }

    pub async fn release(&self, channel: &str) -> Result<Value, BrokerClientError> {
        self.request(
            "release",
            channel,
            json!({}),
            &["released"],
            REQUEST_TIMEOUT,
        )
        .await
    }

    pub async fn write(
        &self,
        channel: &str,
        text: &str,
        line_ending: &str,
    ) -> Result<Value, BrokerClientError> {
        self.write_request(channel, text, line_ending, None).await
    }

    async fn write_request(
        &self,
        channel: &str,
        text: &str,
        line_ending: &str,
        observer_redaction_token: Option<&str>,
    ) -> Result<Value, BrokerClientError> {
        let channel = normalize_channel(channel)?;
        let payload = format!("{text}{}", line_ending_text(line_ending)?);
        let mut fields = json!({"data": {"encoding": "utf8", "value": payload}});
        if let Some(token) = observer_redaction_token {
            fields["observer_redaction"] = json!({"line_token": token});
        }
        let mut response = self
            .request("write", channel, fields, &["write_ack"], REQUEST_TIMEOUT)
            .await?;
        response["cursor"] = Value::from(self.current_cursor(channel).await?);
        Ok(response)
    }

    pub async fn close_channel(&self, channel: &str) -> Result<Value, BrokerClientError> {
        let channel = normalize_channel(channel)?;
        let open = self
            .state
            .lock()
            .await
            .channels
            .get(channel)
            .is_some_and(|history| history.open);
        if !open {
            return Ok(json!({"channel": channel, "closed": true}));
        }
        self.request("close", channel, json!({}), &["closed"], REQUEST_TIMEOUT)
            .await
    }

    pub async fn current_cursor(&self, channel: &str) -> Result<u64, BrokerClientError> {
        let channel = normalize_channel(channel)?;
        Ok(self
            .state
            .lock()
            .await
            .channels
            .get(channel)
            .expect("known channel is initialized")
            .latest_cursor)
    }

    async fn collect(
        &self,
        channel: &str,
        cursor: u64,
        max_units: usize,
    ) -> Result<Value, BrokerClientError> {
        let channel = normalize_channel(channel)?;
        self.state
            .lock()
            .await
            .channels
            .get(channel)
            .expect("known channel is initialized")
            .collect(channel, cursor, max_units)
    }

    pub async fn read(
        &self,
        channel: &str,
        cursor: Option<u64>,
        max_units: usize,
        wait: Duration,
    ) -> Result<Value, BrokerClientError> {
        let channel = normalize_channel(channel)?;
        let (start, open) = {
            let state = self.state.lock().await;
            let history = state
                .channels
                .get(channel)
                .expect("known channel is initialized");
            (cursor.unwrap_or(history.earliest_cursor), history.open)
        };
        let mut result = self.collect(channel, start, max_units).await?;
        if !result["text"].as_str().unwrap_or_default().is_empty() || wait.is_zero() || !open {
            return Ok(result);
        }

        let deadline = Instant::now() + wait;
        loop {
            let notify = {
                self.state
                    .lock()
                    .await
                    .channels
                    .get(channel)
                    .expect("known channel is initialized")
                    .notify
                    .clone()
            };
            let notified = notify.notified();
            tokio::pin!(notified);
            notified.as_mut().enable();

            // Recheck after registering the waiter so data arriving between the
            // first collect and Notify registration cannot be missed.
            result = self.collect(channel, start, max_units).await?;
            if !result["text"].as_str().unwrap_or_default().is_empty() {
                return Ok(result);
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Ok(result);
            }
            if tokio::time::timeout(remaining, &mut notified)
                .await
                .is_err()
            {
                return Ok(result);
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub async fn expect(
        &self,
        channel: &str,
        pattern: &str,
        regex: bool,
        case_sensitive: bool,
        cursor: Option<u64>,
        timeout: Duration,
        max_units: usize,
    ) -> Result<Value, BrokerClientError> {
        let channel = normalize_channel(channel)?;
        let start = cursor.unwrap_or(self.current_cursor(channel).await?);
        let matcher_pattern = if regex {
            pattern.to_owned()
        } else {
            regex::escape(pattern)
        };
        let matcher = RegexBuilder::new(&matcher_pattern)
            .multi_line(true)
            .case_insensitive(!case_sensitive)
            .build()
            .map_err(|error| BrokerClientError::new("invalid_regex", error.to_string()))?;
        let deadline = Instant::now() + timeout;
        let mut search_cursor = start;

        loop {
            let result = self.collect(channel, search_cursor, max_units).await?;
            let text = result["text"].as_str().unwrap_or_default();
            let matched = matcher
                .find(text)
                .map(|found| (utf16_len(&text[..found.start()]), found.as_str().to_owned()));
            if let Some((match_offset, matched)) = matched {
                let match_units = utf16_len(&matched);
                let mut output = result;
                output["matched"] = Value::String(matched);
                output["match_index"] = Value::from(match_offset);
                output["started_cursor"] = Value::from(start);
                output["match_cursor"] = Value::from(search_cursor + match_offset);
                output["next_cursor"] = Value::from(search_cursor + match_offset + match_units);
                return Ok(output);
            }
            if result["truncated"].as_bool() == Some(true) {
                let next = result["next_cursor"].as_u64().unwrap_or(search_cursor);
                let overlap = utf16_len(pattern).clamp(256, 4_096);
                search_cursor = (next.saturating_sub(overlap)).max(search_cursor + 1);
                continue;
            }
            let now = Instant::now();
            if now >= deadline {
                return Err(BrokerClientError::new(
                    "serial_expect_timeout",
                    format!("timed out waiting for {pattern:?}"),
                )
                .with_details(json!({
                    "channel": channel,
                    "cursor": start,
                    "captured": text
                })));
            }
            let notify = {
                self.state
                    .lock()
                    .await
                    .channels
                    .get(channel)
                    .expect("known channel is initialized")
                    .notify
                    .clone()
            };
            let remaining = deadline
                .saturating_duration_since(now)
                .min(Duration::from_millis(250));
            let _ = tokio::time::timeout(remaining, notify.notified()).await;
        }
    }

    pub async fn command(
        &self,
        channel: &str,
        command: &str,
        prompt: &str,
        prompt_regex: bool,
        line_ending: &str,
        timeout: Duration,
    ) -> Result<Value, BrokerClientError> {
        let channel = normalize_channel(channel)?;
        let cursor = self.current_cursor(channel).await?;
        self.claim(channel, "mcp-command").await?;
        let operation = async {
            let write = self.write(channel, command, line_ending).await?;
            let output = self
                .expect(
                    channel,
                    prompt,
                    prompt_regex,
                    true,
                    Some(cursor),
                    timeout,
                    131_072,
                )
                .await?;
            Ok(json!({"channel": channel, "write": write, "output": output}))
        }
        .await;
        finish_claim(operation, self.release(channel).await)
    }

    #[allow(clippy::too_many_arguments)]
    pub async fn shell_command(
        &self,
        channel: &str,
        command: &str,
        prompt: &str,
        prompt_regex: bool,
        require_zero: bool,
        line_ending: &str,
        timeout: Duration,
    ) -> Result<Value, BrokerClientError> {
        let channel = normalize_channel(channel)?;
        let token = Uuid::new_v4().simple().to_string();
        let marker = format!("__LINKR_RC_{token}__");
        let variable = format!("__linkr_rc_{token}");
        let wrapped = format!("{command}\n{variable}=$?\nprintf '{marker}%d\\n' \"${variable}\"");
        let cursor = self.current_cursor(channel).await?;
        self.claim(channel, "mcp-shell-command").await?;
        let operation = async {
            self.write_request(channel, &wrapped, line_ending, Some(&token))
                .await?;
            let marker_pattern = format!("{}([0-9]+)", regex::escape(&marker));
            let exit = self
                .expect(
                    channel,
                    &marker_pattern,
                    true,
                    true,
                    Some(cursor),
                    timeout,
                    131_072,
                )
                .await?;
            let matched = exit["matched"].as_str().unwrap_or_default();
            let exit_code = Regex::new(&marker_pattern)
                .ok()
                .and_then(|pattern| pattern.captures(matched))
                .and_then(|captures| captures.get(1))
                .and_then(|value| value.as_str().parse::<i32>().ok())
                .ok_or_else(|| {
                    BrokerClientError::new(
                        "serial_exit_code_missing",
                        "target shell did not return a valid exit code",
                    )
                })?;
            let shell = self
                .expect(
                    channel,
                    prompt,
                    prompt_regex,
                    true,
                    exit["next_cursor"].as_u64(),
                    timeout,
                    131_072,
                )
                .await?;
            let match_index = exit["match_index"].as_u64().unwrap_or(0) as usize;
            let output_text = redact_internal_shell_text(
                &utf16_prefix(exit["text"].as_str().unwrap_or_default(), match_index),
                &token,
            );
            let result = json!({
                "channel": channel,
                "exit_code": exit_code,
                "output": output_text,
                "next_cursor": shell["next_cursor"]
            });
            if require_zero && exit_code != 0 {
                return Err(BrokerClientError::new(
                    "serial_command_failed",
                    format!("target command exited with status {exit_code}"),
                )
                .with_details(result));
            }
            Ok(result)
        }
        .await;
        finish_claim(operation, self.release(channel).await)
    }

    #[allow(clippy::too_many_arguments)]
    pub async fn login(
        &self,
        channel: &str,
        username: &str,
        password: &str,
        login_prompt: &str,
        password_prompt: &str,
        shell_prompt: &str,
        shell_prompt_regex: bool,
        line_ending: &str,
        timeout: Duration,
    ) -> Result<Value, BrokerClientError> {
        let channel = normalize_channel(channel)?;
        self.claim(channel, "mcp-login").await?;
        let deadline = Instant::now() + timeout;
        let operation = async {
            let snapshot = self.tail(channel, 4_096).await?;
            if matches_tail(
                snapshot["text"].as_str().unwrap_or_default(),
                shell_prompt,
                shell_prompt_regex,
            )? {
                return Ok(json!({
                    "channel": channel,
                    "authenticated": true,
                    "already_authenticated": true,
                    "next_cursor": snapshot["next_cursor"]
                }));
            }

            if !snapshot["text"]
                .as_str()
                .unwrap_or_default()
                .contains(login_prompt)
            {
                let cursor = self.current_cursor(channel).await?;
                self.write(channel, "", line_ending).await?;
                let combined = format!(
                    "{}|{}",
                    regex::escape(login_prompt),
                    if shell_prompt_regex {
                        shell_prompt.to_owned()
                    } else {
                        regex::escape(shell_prompt)
                    }
                );
                let prompt = self
                    .expect(
                        channel,
                        &combined,
                        true,
                        true,
                        Some(cursor),
                        remaining(deadline)?,
                        131_072,
                    )
                    .await?;
                if matches_pattern(
                    prompt["matched"].as_str().unwrap_or_default(),
                    shell_prompt,
                    shell_prompt_regex,
                )? {
                    return Ok(json!({
                        "channel": channel,
                        "authenticated": true,
                        "already_authenticated": true,
                        "next_cursor": prompt["next_cursor"]
                    }));
                }
            }

            let cursor = self.current_cursor(channel).await?;
            self.write(channel, username, line_ending).await?;
            let password_or_shell = format!(
                "{}|{}",
                regex::escape(password_prompt),
                if shell_prompt_regex {
                    shell_prompt.to_owned()
                } else {
                    regex::escape(shell_prompt)
                }
            );
            let next = self
                .expect(
                    channel,
                    &password_or_shell,
                    true,
                    true,
                    Some(cursor),
                    remaining(deadline)?,
                    131_072,
                )
                .await?;
            if matches_pattern(
                next["matched"].as_str().unwrap_or_default(),
                shell_prompt,
                shell_prompt_regex,
            )? {
                return Ok(json!({
                    "channel": channel,
                    "authenticated": true,
                    "password_required": false,
                    "next_cursor": next["next_cursor"]
                }));
            }

            let cursor = self.current_cursor(channel).await?;
            self.write(channel, password, line_ending).await?;
            let shell = self
                .expect(
                    channel,
                    shell_prompt,
                    shell_prompt_regex,
                    true,
                    Some(cursor),
                    remaining(deadline)?,
                    131_072,
                )
                .await
                .map_err(|error| redact_error(error, password))?;
            Ok(json!({
                "channel": channel,
                "authenticated": true,
                "password_required": true,
                "next_cursor": shell["next_cursor"]
            }))
        }
        .await;
        finish_claim(operation, self.release(channel).await)
    }

    async fn tail(&self, channel: &str, max_units: usize) -> Result<Value, BrokerClientError> {
        let channel = normalize_channel(channel)?;
        let cursor = {
            let state = self.state.lock().await;
            let history = state
                .channels
                .get(channel)
                .expect("known channel is initialized");
            history
                .latest_cursor
                .saturating_sub(max_units as u64)
                .max(history.earliest_cursor)
        };
        self.collect(channel, cursor, max_units).await
    }

    pub async fn close(&self) {
        for channel in ["uart0", "uart1"] {
            let (open, claimed) = {
                let state = self.state.lock().await;
                let history = state
                    .channels
                    .get(channel)
                    .expect("known channel is initialized");
                (history.open, history.claimed)
            };
            if !open {
                continue;
            }
            if claimed {
                let _ = self.release(channel).await;
            }
            let _ = self.close_channel(channel).await;
        }
        let _ = self.outgoing.send(Message::Close(None));
    }
}

fn normalize_channel(channel: &str) -> Result<&str, BrokerClientError> {
    broker_channel(Some(channel))
        .ok_or_else(|| BrokerClientError::new("invalid_channel", "channel must be uart0 or uart1"))
}

fn broker_channel(channel: Option<&str>) -> Option<&str> {
    match channel {
        Some("uart0") => Some("uart0"),
        Some("uart1") => Some("uart1"),
        _ => None,
    }
}

fn line_ending_text(value: &str) -> Result<&'static str, BrokerClientError> {
    match value {
        "cr" => Ok("\r"),
        "lf" => Ok("\n"),
        "crlf" => Ok("\r\n"),
        "none" => Ok(""),
        _ => Err(BrokerClientError::new(
            "invalid_line_ending",
            format!("unsupported line ending: {value}"),
        )),
    }
}

fn finish_claim<T>(
    operation: Result<T, BrokerClientError>,
    release: Result<Value, BrokerClientError>,
) -> Result<T, BrokerClientError> {
    match (operation, release) {
        (Ok(value), Ok(_)) => Ok(value),
        (Ok(_), Err(error)) => Err(error),
        (Err(error), _) => Err(error),
    }
}

fn utf16_len(value: &str) -> u64 {
    value.encode_utf16().count() as u64
}

fn utf16_prefix(value: &str, units: usize) -> String {
    String::from_utf16_lossy(&value.encode_utf16().take(units).collect::<Vec<_>>())
}

fn matches_pattern(text: &str, pattern: &str, regex: bool) -> Result<bool, BrokerClientError> {
    if regex {
        Ok(Regex::new(pattern)
            .map_err(|error| BrokerClientError::new("invalid_regex", error.to_string()))?
            .is_match(text))
    } else {
        Ok(text.contains(pattern))
    }
}

fn matches_tail(text: &str, pattern: &str, regex: bool) -> Result<bool, BrokerClientError> {
    if regex {
        matches_pattern(text, pattern, true)
    } else {
        Ok(text.ends_with(pattern))
    }
}

fn remaining(deadline: Instant) -> Result<Duration, BrokerClientError> {
    let remaining = deadline.saturating_duration_since(Instant::now());
    if remaining.is_zero() {
        Err(BrokerClientError::new(
            "serial_expect_timeout",
            "serial login timed out",
        ))
    } else {
        Ok(remaining)
    }
}

fn redact_error(mut error: BrokerClientError, secret: &str) -> BrokerClientError {
    if secret.is_empty() {
        return error;
    }
    error.message = error.message.replace(secret, "[redacted]");
    error.details = redact_value(error.details, secret);
    error
}

fn redact_value(value: Value, secret: &str) -> Value {
    match value {
        Value::String(value) => Value::String(value.replace(secret, "[redacted]")),
        Value::Array(values) => Value::Array(
            values
                .into_iter()
                .map(|value| redact_value(value, secret))
                .collect(),
        ),
        Value::Object(values) => Value::Object(
            values
                .into_iter()
                .map(|(key, value)| (key, redact_value(value, secret)))
                .collect(),
        ),
        value => value,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn history_uses_javascript_compatible_utf16_cursors() {
        let mut history = History::default();
        history.append("A😀B");
        assert_eq!(history.latest_cursor, 4);
        let result = history.collect("uart0", 1, 2).unwrap();
        assert_eq!(result["text"], "😀");
        assert_eq!(result["next_cursor"], 3);
    }

    #[test]
    fn history_rejects_cursors_from_a_newer_or_different_session() {
        let mut history = History::default();
        history.append("boot");
        let error = history.collect("uart0", 5_000, 100).unwrap_err();
        assert_eq!(error.code, "serial_cursor_ahead");
        assert_eq!(error.details["latest_cursor"], 4);
    }

    #[test]
    fn history_continues_from_a_reconnect_checkpoint() {
        let mut history = History::at_cursor(5_000);
        history.append("boot");
        let result = history.collect("uart0", 5_000, 100).unwrap();
        assert_eq!(result["text"], "boot");
        assert_eq!(result["next_cursor"], 5_004);
    }

    #[test]
    fn redacts_passwords_from_nested_errors() {
        let error = BrokerClientError::new("failed", "password secret failed")
            .with_details(json!({"captured": ["secret", {"line": "xsecretx"}]}));
        let redacted = redact_error(error, "secret");
        assert!(!format!("{}{}", redacted.message, redacted.details).contains("secret"));
    }

    #[test]
    fn validates_line_endings() {
        assert_eq!(line_ending_text("crlf").unwrap(), "\r\n");
        assert_eq!(line_ending_text("none").unwrap(), "");
        assert!(line_ending_text("invalid").is_err());
    }
}
