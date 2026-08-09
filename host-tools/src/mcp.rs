use std::{future::Future, sync::Arc, time::Duration};

use anyhow::{Context, Result};
use reqwest::{Client, Method};
use rmcp::{
    handler::server::wrapper::Parameters,
    model::{CallToolResult, ProgressNotificationParam, RequestMetaObject},
    schemars::JsonSchema,
    tool, tool_handler, tool_router, Peer, RoleServer, ServerHandler, ServiceExt,
};
use serde::Deserialize;
use serde_json::{json, Value};
use tokio::sync::Mutex;
use url::Url;

use crate::mcp_serial::{BrokerClientError, SerialBrokerClient};

const MCP_SCHEMA: &str = "radxa-linkr-debugger.mcp.v1";

#[derive(Debug, Clone)]
struct RuntimeError {
    code: String,
    message: String,
    details: Value,
}

impl RuntimeError {
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

    fn apply_retry_policy(mut self, policy: RetryPolicy) -> Self {
        if policy == RetryPolicy::Never {
            if !self.details.is_object() {
                self.details = json!({});
            }
            let details = self
                .details
                .as_object_mut()
                .expect("error details are an object");
            details.insert("retryable".to_owned(), Value::Bool(false));
            details.insert(
                "request_outcome".to_owned(),
                Value::String("unknown".to_owned()),
            );
        }
        self
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RetryPolicy {
    Preserve,
    Never,
}

impl From<BrokerClientError> for RuntimeError {
    fn from(error: BrokerClientError) -> Self {
        Self {
            code: error.code,
            message: error.message,
            details: error.details,
        }
    }
}

#[derive(Clone)]
pub struct LinkrMcpServer {
    api_base: Url,
    serial_url: Url,
    client: Client,
    serial: Arc<Mutex<Option<SerialBrokerClient>>>,
}

impl LinkrMcpServer {
    pub fn new(mut api_base: Url, serial_url: Url) -> Result<Self> {
        if !api_base.path().ends_with('/') {
            let path = format!("{}/", api_base.path());
            api_base.set_path(&path);
        }
        let client = Client::builder()
            .connect_timeout(Duration::from_secs(3))
            .timeout(Duration::from_secs(10))
            .build()
            .context("build MCP HTTP client")?;
        Ok(Self {
            api_base,
            serial_url,
            client,
            serial: Arc::new(Mutex::new(None)),
        })
    }

    async fn board_request(
        &self,
        path: &str,
        method: Method,
        body: Option<Value>,
    ) -> Result<Value, RuntimeError> {
        let read_only = method == Method::GET;
        let url = self
            .api_base
            .join(path)
            .map_err(|error| RuntimeError::new("invalid_gateway_url", error.to_string()))?;
        let mut request = self.client.request(method, url.clone());
        if let Some(body) = body {
            request = request.json(&body);
        }
        let response = request.send().await.map_err(|error| {
            let (code, outcome) = if error.is_connect() {
                ("host_temporarily_unavailable", "not_sent")
            } else if error.is_timeout() {
                ("host_timeout", "unknown")
            } else {
                ("host_request_failed", "unknown")
            };
            RuntimeError::new(code, error.to_string()).with_details(json!({
                "dependency": "linkr_host",
                "endpoint": url.as_str(),
                "retryable": read_only,
                "retry_after_ms": 1_000,
                "request_outcome": outcome
            }))
        })?;
        let status = response.status();
        let text = response
            .text()
            .await
            .map_err(|error| RuntimeError::new("invalid_board_response", error.to_string()))?;
        let value = serde_json::from_str::<Value>(&text).map_err(|_| {
            RuntimeError::new(
                "invalid_board_response",
                format!(
                    "device returned HTTP {} with a non-JSON response",
                    status.as_u16()
                ),
            )
        })?;
        if !status.is_success() || value.get("ok").and_then(Value::as_bool) == Some(false) {
            let error = value.get("error");
            return Err(RuntimeError::new(
                error
                    .and_then(|value| value.get("code"))
                    .and_then(Value::as_str)
                    .map(str::to_owned)
                    .unwrap_or_else(|| format!("http_{}", status.as_u16())),
                error
                    .and_then(|value| value.get("message"))
                    .and_then(Value::as_str)
                    .unwrap_or("device request failed"),
            )
            .with_details(json!({"status": status.as_u16(), "response": value})));
        }
        if value.get("schema").and_then(Value::as_str) != Some("radxa-linkr-debugger.v1") {
            return Err(RuntimeError::new(
                "unsupported_board_schema",
                format!(
                    "expected radxa-linkr-debugger.v1 but received {}",
                    value
                        .get("schema")
                        .and_then(Value::as_str)
                        .unwrap_or("no schema")
                ),
            ));
        }
        Ok(value)
    }

    async fn serial(&self) -> Result<SerialBrokerClient, RuntimeError> {
        let mut serial = self.serial.lock().await;
        let mut checkpoints = None;
        if let Some(client) = serial.as_ref() {
            if !client.is_closed().await {
                return Ok(client.clone());
            }
            checkpoints = Some(client.cursor_checkpoints().await);
        }
        let client = if let Some(checkpoints) = checkpoints.as_ref() {
            SerialBrokerClient::connect_with_cursor_checkpoints(
                self.serial_url.clone(),
                checkpoints,
            )
            .await?
        } else {
            SerialBrokerClient::connect(self.serial_url.clone()).await?
        };
        *serial = Some(client.clone());
        Ok(client)
    }

    async fn execute<F>(&self, name: &str, operation: F) -> CallToolResult
    where
        F: Future<Output = Result<Value, RuntimeError>>,
    {
        self.execute_with_retry_policy(name, RetryPolicy::Preserve, operation)
            .await
    }

    async fn execute_with_retry_policy<F>(
        &self,
        name: &str,
        retry_policy: RetryPolicy,
        operation: F,
    ) -> CallToolResult
    where
        F: Future<Output = Result<Value, RuntimeError>>,
    {
        match operation
            .await
            .map_err(|error| error.apply_retry_policy(retry_policy))
        {
            Ok(result) => success(name, result),
            Err(error) => failure(name, error),
        }
    }

    async fn execute_with_progress<F>(
        &self,
        name: &str,
        activity: &str,
        meta: RequestMetaObject,
        peer: Peer<RoleServer>,
        retry_policy: RetryPolicy,
        operation: F,
    ) -> CallToolResult
    where
        F: Future<Output = Result<Value, RuntimeError>>,
    {
        let started = tokio::time::Instant::now();
        let progress_token = meta.get_progress_token();
        if let Some(token) = progress_token.as_ref() {
            let _ = peer
                .notify_progress(
                    ProgressNotificationParam::new(token.clone(), 0.0)
                        .with_message(activity.to_owned()),
                )
                .await;
        }

        tokio::pin!(operation);
        let result = if let Some(token) = progress_token {
            let mut ticker = tokio::time::interval(Duration::from_secs(1));
            ticker.tick().await;
            loop {
                tokio::select! {
                    result = operation.as_mut() => break result,
                    _ = ticker.tick() => {
                        let elapsed = started.elapsed().as_secs_f64();
                        let _ = peer
                            .notify_progress(
                                ProgressNotificationParam::new(token.clone(), elapsed)
                                    .with_message(format!("{activity} · {:.0}s", elapsed)),
                            )
                            .await;
                    }
                }
            }
        } else {
            operation.as_mut().await
        };

        let result = result.map_err(|error| error.apply_retry_policy(retry_policy));
        let succeeded = result.is_ok();
        if let Some(token) = meta.get_progress_token() {
            let elapsed = started.elapsed().as_secs_f64();
            let state = if succeeded { "completed" } else { "failed" };
            let _ = peer
                .notify_progress(
                    ProgressNotificationParam::new(token, elapsed)
                        .with_message(format!("{activity} · {state} · {:.1}s", elapsed)),
                )
                .await;
        }
        match result {
            Ok(value) => success(name, value),
            Err(error) => failure(name, error),
        }
    }

    pub async fn close(&self) {
        if let Some(serial) = self.serial.lock().await.take() {
            serial.close().await;
        }
    }
}

#[tool_router]
impl LinkrMcpServer {
    #[tool(
        name = "linkr_board_status",
        description = "Read a compact power, route and health summary without changing hardware. Use detail=full only when GPIO and complete diagnostics are needed."
    )]
    async fn board_status(&self, Parameters(args): Parameters<BoardStatusArgs>) -> CallToolResult {
        self.execute("linkr_board_status", async {
            let status = self.board_request("status", Method::GET, None).await?;
            Ok(if args.detail == Detail::Full {
                status
            } else {
                summarize_board_status(&status)
            })
        })
        .await
    }

    #[tool(
        name = "linkr_adc_read",
        description = "Read the three power-rail ADC current monitors without changing hardware."
    )]
    async fn adc_read(&self) -> CallToolResult {
        self.execute(
            "linkr_adc_read",
            self.board_request("adc/read", Method::GET, None),
        )
        .await
    }

    #[tool(
        name = "linkr_power_set",
        description = "Turn a target 5V, 12V or 20V rail on or off. confirm must be true because this immediately changes target hardware power."
    )]
    async fn power_set(&self, Parameters(args): Parameters<PowerSetArgs>) -> CallToolResult {
        self.execute("linkr_power_set", async {
            if !args.confirm {
                return Err(RuntimeError::new(
                    "confirmation_required",
                    "confirm must be true before changing a target power rail",
                ));
            }
            self.board_request(
                &format!("power/{}", args.name.as_str()),
                Method::PUT,
                Some(json!({"state": args.state.as_str()})),
            )
            .await
        })
        .await
    }

    #[tool(
        name = "linkr_switch_route",
        description = "Route SD, target USB or UART VIO. confirm must be true because a route change can disconnect or electrically mismatch the target."
    )]
    async fn switch_route(&self, Parameters(args): Parameters<SwitchRouteArgs>) -> CallToolResult {
        self.execute("linkr_switch_route", async {
            if !args.confirm {
                return Err(RuntimeError::new(
                    "confirmation_required",
                    "confirm must be true before changing a target route",
                ));
            }
            if !args.name.accepts(&args.route) {
                return Err(RuntimeError::new(
                    "invalid_route",
                    format!(
                        "{} does not accept route {}",
                        args.name.as_str(),
                        args.route
                    ),
                ));
            }
            self.board_request(
                &format!("switch/{}", args.name.as_str()),
                Method::PUT,
                Some(json!({"route": args.route})),
            )
            .await
        })
        .await
    }

    #[tool(
        name = "linkr_serial_connect",
        description = "Subscribe to UART0 or UART1 through the shared host Serial Broker. Existing Web readers remain connected."
    )]
    async fn serial_connect(
        &self,
        Parameters(args): Parameters<SerialConnectArgs>,
    ) -> CallToolResult {
        self.execute_with_retry_policy("linkr_serial_connect", RetryPolicy::Never, async {
            Ok(self
                .serial()
                .await?
                .open(args.channel.as_str(), args.baud)
                .await?)
        })
        .await
    }

    #[tool(
        name = "linkr_serial_status",
        description = "Read connection, baud, subscriber and current write-owner state without opening or subscribing to the UART."
    )]
    async fn serial_status(
        &self,
        Parameters(args): Parameters<SerialChannelArgs>,
    ) -> CallToolResult {
        self.execute("linkr_serial_status", async {
            Ok(self.serial().await?.status(args.channel.as_str()).await?)
        })
        .await
    }

    #[tool(
        name = "linkr_serial_read",
        description = "Read retained UART text after a cursor. Reuse next_cursor to avoid repeatedly returning the whole log."
    )]
    async fn serial_read(&self, Parameters(args): Parameters<SerialReadArgs>) -> CallToolResult {
        self.execute("linkr_serial_read", async {
            Ok(self
                .serial()
                .await?
                .read(
                    args.channel.as_str(),
                    args.cursor,
                    args.max_chars.clamp(1, 65_536),
                    Duration::from_millis(args.wait_ms.min(60_000)),
                )
                .await?)
        })
        .await
    }

    #[tool(
        name = "linkr_serial_expect",
        description = "Wait for text or a regular expression after a cursor and return only the bounded captured window."
    )]
    async fn serial_expect(
        &self,
        Parameters(args): Parameters<SerialExpectArgs>,
        meta: RequestMetaObject,
        peer: Peer<RoleServer>,
    ) -> CallToolResult {
        self.execute_with_progress(
            "linkr_serial_expect",
            "Waiting for target UART output",
            meta,
            peer,
            RetryPolicy::Preserve,
            async {
                if args.pattern.is_empty() {
                    return Err(RuntimeError::new(
                        "invalid_pattern",
                        "pattern must not be empty",
                    ));
                }
                Ok(self
                    .serial()
                    .await?
                    .expect(
                        args.channel.as_str(),
                        &args.pattern,
                        args.regex,
                        args.case_sensitive,
                        args.cursor,
                        Duration::from_millis(args.timeout_ms.clamp(1, 600_000)),
                        args.max_chars.clamp(1, 262_144),
                    )
                    .await?)
            },
        )
        .await
    }

    #[tool(
        name = "linkr_serial_write",
        description = "Write text to an already connected target UART. A short Broker claim prevents another client from interleaving bytes."
    )]
    async fn serial_write(&self, Parameters(args): Parameters<SerialWriteArgs>) -> CallToolResult {
        self.execute_with_retry_policy("linkr_serial_write", RetryPolicy::Never, async {
            let serial = self.serial().await?;
            if !args.exclusive {
                return Ok(serial
                    .write(args.channel.as_str(), &args.text, args.line_ending.as_str())
                    .await?);
            }
            serial.claim(args.channel.as_str(), "mcp-write").await?;
            let operation = serial
                .write(args.channel.as_str(), &args.text, args.line_ending.as_str())
                .await;
            let release = serial.release(args.channel.as_str()).await;
            match (operation, release) {
                (Ok(value), Ok(_)) => Ok(value),
                (Ok(_), Err(error)) | (Err(error), _) => Err(error.into()),
            }
        })
        .await
    }

    #[tool(
        name = "linkr_serial_command",
        description = "Claim an already connected UART, send one command, wait for a prompt, return the bounded output, and release the claim."
    )]
    async fn serial_command(
        &self,
        Parameters(args): Parameters<SerialCommandArgs>,
        meta: RequestMetaObject,
        peer: Peer<RoleServer>,
    ) -> CallToolResult {
        self.execute_with_progress(
            "linkr_serial_command",
            "Running target UART command",
            meta,
            peer,
            RetryPolicy::Never,
            async {
                Ok(self
                    .serial()
                    .await?
                    .command(
                        args.channel.as_str(),
                        &args.command,
                        &args.prompt,
                        args.prompt_regex,
                        args.line_ending.as_str(),
                        Duration::from_millis(args.timeout_ms.clamp(1, 600_000)),
                    )
                    .await?)
            },
        )
        .await
    }

    #[tool(
        name = "linkr_serial_shell_command",
        description = "Claim an authenticated POSIX shell, run one command or script, capture its exit code and bounded output, wait for the prompt, then release the UART."
    )]
    async fn serial_shell_command(
        &self,
        Parameters(args): Parameters<SerialShellCommandArgs>,
        meta: RequestMetaObject,
        peer: Peer<RoleServer>,
    ) -> CallToolResult {
        self.execute_with_progress(
            "linkr_serial_shell_command",
            "Running target shell command",
            meta,
            peer,
            RetryPolicy::Never,
            async {
                Ok(self
                    .serial()
                    .await?
                    .shell_command(
                        args.channel.as_str(),
                        &args.command,
                        &args.prompt,
                        args.prompt_regex,
                        args.require_zero,
                        args.line_ending.as_str(),
                        Duration::from_millis(args.timeout_ms.clamp(1, 600_000)),
                    )
                    .await?)
            },
        )
        .await
    }

    #[tool(
        name = "linkr_serial_login",
        description = "Claim an already connected UART, detect an existing shell or complete username/password login, and release it. Password text is never returned."
    )]
    async fn serial_login(
        &self,
        Parameters(args): Parameters<SerialLoginArgs>,
        meta: RequestMetaObject,
        peer: Peer<RoleServer>,
    ) -> CallToolResult {
        self.execute_with_progress(
            "linkr_serial_login",
            "Waiting for target UART login",
            meta,
            peer,
            RetryPolicy::Never,
            async {
                Ok(self
                    .serial()
                    .await?
                    .login(
                        args.channel.as_str(),
                        &args.username,
                        &args.password,
                        &args.login_prompt,
                        &args.password_prompt,
                        &args.shell_prompt,
                        args.shell_prompt_regex,
                        args.line_ending.as_str(),
                        Duration::from_millis(args.timeout_ms.clamp(1, 600_000)),
                    )
                    .await?)
            },
        )
        .await
    }

    #[tool(
        name = "linkr_serial_disconnect",
        description = "Release this MCP client's claim and subscription without disconnecting other Web or Agent subscribers."
    )]
    async fn serial_disconnect(
        &self,
        Parameters(args): Parameters<SerialChannelArgs>,
    ) -> CallToolResult {
        self.execute_with_retry_policy("linkr_serial_disconnect", RetryPolicy::Never, async {
            let serial = self.serial.lock().await.clone();
            let Some(serial) = serial else {
                return Ok(json!({
                    "channel": args.channel.as_str(),
                    "closed": true,
                    "already_disconnected": true
                }));
            };
            Ok(serial.close_channel(args.channel.as_str()).await?)
        })
        .await
    }
}

#[tool_handler(
    name = "radxa-linkr-debugger",
    instructions = "Operate the local Radxa Linkr Debugger through bounded board and shared Serial Broker tools. Read status before changing hardware state."
)]
impl ServerHandler for LinkrMcpServer {}

pub async fn serve_stdio(api_base: Url, serial_url: Url) -> Result<()> {
    let server = LinkrMcpServer::new(api_base, serial_url)?;
    let service = server
        .clone()
        .serve(rmcp::transport::stdio())
        .await
        .context("start Linkr MCP stdio service")?;
    service
        .waiting()
        .await
        .context("run Linkr MCP stdio service")?;
    server.close().await;
    Ok(())
}

fn success(tool: &str, result: Value) -> CallToolResult {
    CallToolResult::structured(json!({
        "schema": MCP_SCHEMA,
        "ok": true,
        "tool": tool,
        "result": result
    }))
}

fn failure(tool: &str, error: RuntimeError) -> CallToolResult {
    let mut output = json!({
        "schema": MCP_SCHEMA,
        "ok": false,
        "tool": tool,
        "error": {
            "code": error.code,
            "message": error.message
        }
    });
    if error
        .details
        .as_object()
        .is_some_and(|details| !details.is_empty())
    {
        output["error"]["details"] = error.details;
    }
    CallToolResult::structured_error(output)
}

fn summarize_board_status(status: &Value) -> Value {
    let power_outputs = status
        .get("power_outputs")
        .and_then(Value::as_array)
        .map(|outputs| {
            outputs
                .iter()
                .map(|output| {
                    json!({
                        "name": output.get("name"),
                        "state": output.get("state"),
                        "value": output.get("value")
                    })
                })
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    let switches = status
        .get("switches")
        .and_then(Value::as_object)
        .map(|switches| {
            switches
                .iter()
                .map(|(name, value)| (name.clone(), json!({"route": value.get("route")})))
                .collect::<serde_json::Map<_, _>>()
        })
        .unwrap_or_default();
    let adc_channels = status
        .get("adc_channels")
        .and_then(Value::as_array)
        .map(|channels| {
            channels
                .iter()
                .map(|channel| {
                    json!({
                        "name": channel.get("name"),
                        "unit": channel.get("unit")
                    })
                })
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    let monitoring = status.get("board_monitoring");
    json!({
        "schema": status.get("schema"),
        "ok": status.get("ok"),
        "command": status.get("command"),
        "project": status.get("project"),
        "mcu": status.get("mcu"),
        "usb": status.get("usb"),
        "power_outputs": power_outputs,
        "switches": switches,
        "adc_channels": adc_channels,
        "watchdog": status.get("watchdog").map(|watchdog| json!({
            "supported": watchdog.get("supported"),
            "healthy": watchdog.get("healthy"),
            "armed": watchdog.get("armed")
        })),
        "board_monitoring": monitoring.map(|monitoring| json!({
            "temperature": monitoring.get("temperature"),
            "runtime": monitoring.get("runtime"),
            "cpu": monitoring.get("cpu"),
            "memory": monitoring.get("memory").map(|memory| json!({
                "available": memory.get("available"),
                "current_pressure_pct_x100": memory.pointer("/current_pressure/pressure_pct_x100"),
                "peak_pressure_pct_x100": memory.pointer("/peak_pressure/pressure_pct_x100")
            }))
        })),
        "power_capture_protocol": status.get("power_capture_protocol")
    })
}

#[derive(Debug, Clone, Deserialize, JsonSchema, PartialEq, Eq, Default)]
#[serde(rename_all = "lowercase")]
enum Detail {
    #[default]
    Summary,
    Full,
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
struct BoardStatusArgs {
    #[serde(default)]
    detail: Detail,
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
enum PowerState {
    On,
    Off,
}

impl PowerState {
    fn as_str(&self) -> &'static str {
        match self {
            Self::On => "on",
            Self::Off => "off",
        }
    }
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
enum PowerName {
    #[serde(rename = "5v_out")]
    Five,
    #[serde(rename = "12v_out")]
    Twelve,
    #[serde(rename = "20v_out")]
    Twenty,
}

impl PowerName {
    fn as_str(&self) -> &'static str {
        match self {
            Self::Five => "5v_out",
            Self::Twelve => "12v_out",
            Self::Twenty => "20v_out",
        }
    }
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
struct PowerSetArgs {
    name: PowerName,
    state: PowerState,
    confirm: bool,
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
enum SwitchName {
    Sd,
    Usb,
    Vin,
}

impl SwitchName {
    fn as_str(&self) -> &'static str {
        match self {
            Self::Sd => "sd",
            Self::Usb => "usb",
            Self::Vin => "vin",
        }
    }

    fn accepts(&self, route: &str) -> bool {
        match self {
            Self::Sd => matches!(route, "target" | "usb-reader"),
            Self::Usb => matches!(route, "target" | "pc"),
            Self::Vin => matches!(route, "1.8v" | "3.3v"),
        }
    }
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
struct SwitchRouteArgs {
    name: SwitchName,
    route: String,
    confirm: bool,
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
enum SerialChannel {
    Uart0,
    Uart1,
}

impl SerialChannel {
    fn as_str(&self) -> &'static str {
        match self {
            Self::Uart0 => "uart0",
            Self::Uart1 => "uart1",
        }
    }
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
struct SerialConnectArgs {
    channel: SerialChannel,
    #[serde(default = "default_baud")]
    baud: u32,
}

fn default_baud() -> u32 {
    115_200
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
struct SerialChannelArgs {
    channel: SerialChannel,
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
struct SerialReadArgs {
    channel: SerialChannel,
    cursor: Option<u64>,
    #[serde(default = "default_max_chars")]
    max_chars: usize,
    #[serde(default)]
    wait_ms: u64,
}

fn default_max_chars() -> usize {
    32_768
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
struct SerialExpectArgs {
    channel: SerialChannel,
    pattern: String,
    #[serde(default)]
    regex: bool,
    #[serde(default = "default_true")]
    case_sensitive: bool,
    cursor: Option<u64>,
    #[serde(default = "default_timeout_ms")]
    timeout_ms: u64,
    #[serde(default = "default_expect_chars")]
    max_chars: usize,
}

fn default_true() -> bool {
    true
}

fn default_timeout_ms() -> u64 {
    30_000
}

fn default_expect_chars() -> usize {
    131_072
}

#[derive(Debug, Clone, Deserialize, JsonSchema, Default)]
#[serde(rename_all = "lowercase")]
enum LineEnding {
    Cr,
    Lf,
    Crlf,
    #[default]
    None,
}

impl LineEnding {
    fn as_str(&self) -> &'static str {
        match self {
            Self::Cr => "cr",
            Self::Lf => "lf",
            Self::Crlf => "crlf",
            Self::None => "none",
        }
    }
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
struct SerialWriteArgs {
    channel: SerialChannel,
    text: String,
    #[serde(default)]
    line_ending: LineEnding,
    #[serde(default = "default_true")]
    exclusive: bool,
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
struct SerialCommandArgs {
    channel: SerialChannel,
    command: String,
    prompt: String,
    #[serde(default)]
    prompt_regex: bool,
    #[serde(default = "default_cr")]
    line_ending: LineEnding,
    #[serde(default = "default_timeout_ms")]
    timeout_ms: u64,
}

fn default_cr() -> LineEnding {
    LineEnding::Cr
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
struct SerialShellCommandArgs {
    channel: SerialChannel,
    command: String,
    #[serde(default = "default_shell_prompt")]
    prompt: String,
    #[serde(default = "default_true")]
    prompt_regex: bool,
    #[serde(default = "default_true")]
    require_zero: bool,
    #[serde(default = "default_cr")]
    line_ending: LineEnding,
    #[serde(default = "default_timeout_ms")]
    timeout_ms: u64,
}

fn default_shell_prompt() -> String {
    "[#$>]\\s*$".to_owned()
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
struct SerialLoginArgs {
    channel: SerialChannel,
    username: String,
    #[serde(default)]
    password: String,
    #[serde(default = "default_login_prompt")]
    login_prompt: String,
    #[serde(default = "default_password_prompt")]
    password_prompt: String,
    #[serde(default = "default_shell_prompt")]
    shell_prompt: String,
    #[serde(default = "default_true")]
    shell_prompt_regex: bool,
    #[serde(default = "default_cr")]
    line_ending: LineEnding,
    #[serde(default = "default_timeout_ms")]
    timeout_ms: u64,
}

fn default_login_prompt() -> String {
    "login:".to_owned()
}

fn default_password_prompt() -> String {
    "Password:".to_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn status_summary_omits_gpio_details() {
        let full = json!({
            "schema": "radxa-linkr-debugger.v1",
            "ok": true,
            "command": "status",
            "power_outputs": [{"name": "5v_out", "state": "off", "value": 0, "signal": "GP05"}],
            "switches": {"sd": {"route": "target", "routes": ["target", "usb-reader"]}},
            "gpios": [{"name": "GP7"}]
        });
        let summary = summarize_board_status(&full);
        assert!(summary.get("gpios").is_none());
        assert_eq!(summary["switches"]["sd"], json!({"route": "target"}));
        assert_eq!(
            summary["power_outputs"][0],
            json!({"name": "5v_out", "state": "off", "value": 0})
        );
    }

    #[test]
    fn switch_routes_are_constrained_by_switch_name() {
        assert!(SwitchName::Sd.accepts("usb-reader"));
        assert!(!SwitchName::Sd.accepts("pc"));
        assert!(SwitchName::Vin.accepts("1.8v"));
    }

    #[test]
    fn api_base_without_trailing_slash_keeps_the_version_prefix() {
        let server = LinkrMcpServer::new(
            Url::parse("http://127.0.0.1:8787/api/v1").unwrap(),
            Url::parse("ws://127.0.0.1:8787/serial").unwrap(),
        )
        .unwrap();
        assert_eq!(
            server.api_base.join("status").unwrap().as_str(),
            "http://127.0.0.1:8787/api/v1/status"
        );
    }
}
