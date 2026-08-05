// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::adc::AdcReading;
use crate::client::{BoardRequest, BoardTransport};
use crate::monitoring::{format_monitoring_summary, BoardMonitoring};
use crate::ws_client::WsStatusSnapshot;
use anyhow::Result;
use crossterm::event::{self, Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers};
use crossterm::execute;
use crossterm::terminal::{
    disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen,
};
use ratatui::backend::CrosstermBackend;
use ratatui::layout::{Alignment, Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::symbols;
use ratatui::text::{Line, Span, Text};
use ratatui::widgets::{
    Block, Borders, Paragraph, Scrollbar, ScrollbarOrientation, ScrollbarState, Sparkline,
};
use ratatui::Terminal;
use reqwest::Method;
use std::collections::BTreeMap;
use std::io::{self, Stdout};
use std::time::{Duration, Instant};

mod config_io;
#[cfg(test)]
mod config_io_tests;
#[cfg(test)]
mod config_key_tests;
mod config_render;
#[cfg(test)]
mod config_render_tests;
mod config_result;
#[cfg(test)]
mod config_result_tests;
mod config_state;
#[cfg(test)]
mod config_state_tests;

use config_io::ConfigWorker;
use config_render::{append_saved_config_lines, render_confirmation};
use config_state::{ConfigRequest, SavedConfigState};

pub const TUI_HISTORY_LIMIT: usize = 240;
pub const TUI_POLL_INTERVAL: Duration = Duration::from_nanos(16_666_667);

#[derive(Debug)]
pub struct TuiActionMsg {
    pub status: String,
    pub err: Option<String>,
}

const HTTP_POLL_INTERVAL: Duration = Duration::from_secs(2);
const CONFIRM_TIMEOUT: Duration = Duration::from_secs(3);
const TARGET_RECOVERY_TIMEOUT: Duration = Duration::from_secs(5);
const RECOVERY_MODES: &[&str] = &["rockchip-maskrom", "qualcomm-edl"];
const RECOVERY_RAILS: &[&str] = &["5v_out", "12v_out", "20v_out"];

#[derive(Debug, Clone, Default)]
struct TuiSwitchState {
    name: String,
    desired_route: String,
    actual_route: String,
    routes: Vec<String>,
    requires_confirm: bool,
    pending_route: Option<String>,
    pending_until: Option<Instant>,
}

pub struct TuiModel {
    pub base_url: String,
    pub timeout: Duration,
    pub width: usize,
    pub height: usize,
    pub paused: bool,
    pub err: Option<String>,
    pub last_http_poll: Option<Instant>,
    pub status: String,
    pub history: std::collections::HashMap<String, Vec<i32>>,
    pub latest: std::collections::HashMap<String, AdcReading>,
    pub control_idx: usize,
    pub scroll_offset: usize,
    pub power_states: std::collections::HashMap<String, bool>,
    switches: BTreeMap<String, TuiSwitchState>,
    pub switch_confirm_active: bool,
    pub switch_confirm_start: Option<Instant>,
    pub switch_confirm_kind: String,
    pub switch_confirm_target: String,
    pub recovery_mode: String,
    pub recovery_rail: String,
    pub recovery_confirm_active: bool,
    pub recovery_confirm_start: Option<Instant>,
    pub gpio_names: Vec<String>,
    pub gpio_notes: std::collections::HashMap<String, String>,
    pub gpio_levels: std::collections::HashMap<String, bool>,
    pub gpio_is_input: std::collections::HashMap<String, bool>,
    pub monitoring: BoardMonitoring,
    saved_config: SavedConfigState,
    config_worker: ConfigWorker,
    pub closed: bool,
    pub channel_ids: Vec<String>,
}

impl TuiModel {
    fn new(base_url: String, timeout: Duration) -> Self {
        Self {
            base_url: base_url.clone(),
            timeout,
            width: 0,
            height: 0,
            paused: false,
            err: None,
            last_http_poll: None,
            status: "HTTP mode".to_string(),
            history: std::collections::HashMap::new(),
            latest: std::collections::HashMap::new(),
            control_idx: 0,
            scroll_offset: 0,
            power_states: std::collections::HashMap::new(),
            switches: BTreeMap::new(),
            switch_confirm_active: false,
            switch_confirm_start: None,
            switch_confirm_kind: String::new(),
            switch_confirm_target: String::new(),
            recovery_mode: RECOVERY_MODES[0].to_string(),
            recovery_rail: RECOVERY_RAILS[0].to_string(),
            recovery_confirm_active: false,
            recovery_confirm_start: None,
            gpio_names: Vec::new(),
            gpio_notes: std::collections::HashMap::new(),
            gpio_levels: std::collections::HashMap::new(),
            gpio_is_input: std::collections::HashMap::new(),
            monitoring: BoardMonitoring::default(),
            saved_config: SavedConfigState::default(),
            config_worker: ConfigWorker::new(),
            closed: false,
            channel_ids: vec![
                "5v_out".to_string(),
                "12v_out".to_string(),
                "20v_out".to_string(),
            ],
        }
    }

    fn apply_status_snapshot(&mut self, snapshot: WsStatusSnapshot) -> bool {
        let config_changed = self.saved_config.observe_summary(snapshot.config);
        for output in snapshot.power_outputs {
            self.power_states.insert(
                output.name.clone(),
                output.value != 0 || output.state == "on",
            );
        }
        let now = Instant::now();
        let mut switches = BTreeMap::new();
        for (name, info) in snapshot.switches {
            let previous = self.switches.remove(&name);
            let (desired_route, pending_route, pending_until) = match previous {
                Some(previous)
                    if previous.pending_route.as_deref() == Some(info.route.as_str()) =>
                {
                    (info.route.clone(), None, None)
                }
                Some(previous) if previous.pending_until.is_some_and(|until| now < until) => (
                    previous.desired_route,
                    previous.pending_route,
                    previous.pending_until,
                ),
                _ => (info.route.clone(), None, None),
            };
            switches.insert(
                name.clone(),
                TuiSwitchState {
                    name,
                    desired_route,
                    actual_route: info.route,
                    routes: info.routes,
                    requires_confirm: info.requires_confirm,
                    pending_route,
                    pending_until,
                },
            );
        }
        self.switches = switches;
        self.gpio_names = snapshot
            .gpios
            .iter()
            .filter(|gpio| gpio.note != "CON_MAS")
            .map(|gpio| gpio.name.clone())
            .collect();
        self.gpio_notes.clear();
        self.gpio_levels.clear();
        self.gpio_is_input.clear();
        for gpio in &snapshot.gpios {
            if gpio.note == "CON_MAS" {
                continue;
            }
            self.gpio_notes.insert(gpio.name.clone(), gpio.note.clone());
            self.gpio_levels
                .insert(gpio.name.clone(), gpio.value.unwrap_or(0) != 0);
            self.gpio_is_input
                .insert(gpio.name.clone(), gpio.direction == "input");
        }
        self.monitoring = snapshot.board_monitoring;
        config_changed
    }

    fn apply_adc_response(&mut self, readings: Vec<AdcReading>) {
        for reading in readings {
            if reading.kind != crate::adc::AdcKind::Current {
                continue;
            }
            if let Some(power_enabled) = reading.power_enabled {
                self.power_states
                    .insert(reading.name.clone(), power_enabled);
            }
            let ma = current_milliamp_estimate(&reading);
            let series = self.history.entry(reading.name.clone()).or_default();
            series.push(ma);
            if series.len() > TUI_HISTORY_LIMIT {
                let drain = series.len() - TUI_HISTORY_LIMIT;
                series.drain(0..drain);
            }
            self.latest.insert(reading.name.clone(), reading);
        }
    }

    fn apply_action_msg(&mut self, msg: TuiActionMsg) {
        self.err = msg.err.clone();
        if let Some(err) = msg.err {
            self.status = err;
        } else {
            self.status = msg.status;
        }
    }
}

pub fn run_tui<TClient>(client: TClient, base_url: String, timeout: Duration) -> Result<u8>
where
    TClient: BoardTransport,
{
    let _ = client.base_url();
    let mut model = TuiModel::new(base_url, timeout);

    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen)?;
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;

    let result = event_loop(&mut model, &mut terminal);

    disable_raw_mode()?;
    execute!(terminal.backend_mut(), LeaveAlternateScreen)?;
    terminal.show_cursor()?;
    result
}

fn event_loop(
    model: &mut TuiModel,
    terminal: &mut Terminal<CrosstermBackend<Stdout>>,
) -> Result<u8> {
    let mut last_tick = Instant::now() - TUI_POLL_INTERVAL;

    loop {
        if last_tick.elapsed() >= TUI_POLL_INTERVAL {
            on_time_tick(model)?;
            last_tick = Instant::now();
        }

        terminal.draw(|frame| {
            model.width = frame.area().width as usize;
            model.height = frame.area().height as usize;
            render_ui(frame, model);
        })?;

        if event::poll(Duration::from_millis(50))? {
            if let Event::Key(key) = event::read()? {
                if handle_key(model, key)? {
                    return Ok(0);
                }
            }
        }
    }
}

fn handle_key(model: &mut TuiModel, key: KeyEvent) -> Result<bool> {
    if key.kind != KeyEventKind::Press {
        return Ok(false);
    }

    match (key.code, key.modifiers) {
        (KeyCode::Char('q'), _) | (KeyCode::Char('c'), KeyModifiers::CONTROL) => {
            model.closed = true;
            return Ok(true);
        }
        _ => {}
    }

    if model.saved_config.confirmation().is_some() {
        match key.code {
            KeyCode::Enter => {
                if let Some(request) = model.saved_config.confirm() {
                    start_config_request(model, request);
                }
            }
            KeyCode::Esc => {
                model.saved_config.cancel_confirmation();
                model.status = "Saved Config cancelled".to_string();
            }
            _ => {}
        }
        return Ok(false);
    }

    if key.code == KeyCode::Esc && model.saved_config.error.is_some() {
        model.saved_config.dismiss_error();
        model.status = "Saved Config error dismissed".to_string();
        return Ok(false);
    }

    if model.saved_config.focused {
        match key.code {
            KeyCode::Esc | KeyCode::Char('c') => {
                model.saved_config.blur();
                return Ok(false);
            }
            KeyCode::Up | KeyCode::Char('k') => {
                model.saved_config.move_cursor(-1);
                return Ok(false);
            }
            KeyCode::Down | KeyCode::Char('j') => {
                model.saved_config.move_cursor(1);
                return Ok(false);
            }
            KeyCode::Enter | KeyCode::Char(' ') => {
                model.saved_config.toggle_current();
                return Ok(false);
            }
            KeyCode::Left
            | KeyCode::Right
            | KeyCode::Char('h')
            | KeyCode::Char('l')
            | KeyCode::Tab => return Ok(false),
            _ => {}
        }
    }

    if model.recovery_confirm_active {
        match key.code {
            KeyCode::Enter | KeyCode::Char(' ') => {}
            _ => {
                model.recovery_confirm_active = false;
                model.recovery_confirm_start = None;
                model.status = "Target recovery cancelled".to_string();
                return Ok(false);
            }
        }
    }

    if model.switch_confirm_active {
        match key.code {
            KeyCode::Enter | KeyCode::Char(' ') => {}
            _ => {
                model.switch_confirm_active = false;
                model.switch_confirm_start = None;
                model.status = "Switch cancelled".to_string();
                return Ok(false);
            }
        }
    }

    match (key.code, key.modifiers) {
        (KeyCode::Char('p'), _) => {
            model.paused = !model.paused;
            if model.paused {
                model.status = "Paused".to_string();
            } else {
                model.status = "Resumed".to_string();
            }
        }
        (KeyCode::Char('r'), _) => {
            model.status = "Refreshing…".to_string();
            poll_http(model)?;
            if let Some(request) = model.saved_config.request_refresh() {
                start_config_request(model, request);
            }
        }
        (KeyCode::Char('c'), _) => {
            model.saved_config.focus();
            if model.saved_config.focused {
                model.status = "Saved Config focused".to_string();
            }
        }
        (KeyCode::Char('s'), _) => {
            if let Some(request) = model.saved_config.request_save() {
                start_config_request(model, request);
            }
        }
        (KeyCode::Char('x'), _) => {
            if let Some(request) = model.saved_config.request_clear() {
                start_config_request(model, request);
            }
        }
        (KeyCode::Up, _) | (KeyCode::Char('k'), _) => {
            model.control_idx = move_control_selection(model, -1, 0);
        }
        (KeyCode::Down, _) | (KeyCode::Char('j'), _) => {
            model.control_idx = move_control_selection(model, 1, 0);
        }
        (KeyCode::Left, _) | (KeyCode::Char('h'), _) => {
            model.control_idx = move_control_selection(model, 0, -1);
        }
        (KeyCode::Right, _) | (KeyCode::Char('l'), _) | (KeyCode::Tab, _) => {
            model.control_idx = move_control_selection(model, 0, 1);
        }
        (KeyCode::Enter, _) | (KeyCode::Char(' '), _) => {
            if let Some(item) = control_items(model).get(model.control_idx).cloned() {
                match item {
                    ControlItem::Power(target) => {
                        let current_state = *model.power_states.get(&target).unwrap_or(&false);
                        let next_state = !current_state;
                        let action = perform_control_action(
                            &model.base_url,
                            model.timeout,
                            &target,
                            current_state,
                        )?;
                        model.power_states.insert(target, next_state);
                        model.apply_action_msg(action);
                    }
                    ControlItem::Gpio(gpio) => {
                        let current_state = *model.gpio_levels.get(&gpio).unwrap_or(&false);
                        let next_state = !current_state;
                        let action =
                            set_gpio_output(&model.base_url, model.timeout, &gpio, next_state)?;
                        model.gpio_levels.insert(gpio.clone(), next_state);
                        model.gpio_is_input.insert(gpio, false);
                        model.apply_action_msg(action);
                    }
                    ControlItem::Switch(name) => {
                        let next_route = model.switches.get(&name).and_then(next_switch_route);
                        let requires_confirm = model
                            .switches
                            .get(&name)
                            .map(|state| state.requires_confirm)
                            .unwrap_or(false);
                        if let Some(next_route) = next_route {
                            if requires_confirm
                                && (!model.switch_confirm_active
                                    || model.switch_confirm_kind != name)
                            {
                                model.switch_confirm_active = true;
                                model.switch_confirm_start = Some(Instant::now());
                                model.switch_confirm_kind = name.clone();
                                model.switch_confirm_target = next_route;
                                model.status = format!(
                                    "Press Enter again within 3s to confirm switch {} to {}",
                                    name, model.switch_confirm_target
                                );
                            } else if (!requires_confirm && !model.switch_confirm_active)
                                || (requires_confirm
                                    && model.switch_confirm_active
                                    && model.switch_confirm_kind == name)
                            {
                                let route = if requires_confirm {
                                    model.switch_confirm_target.clone()
                                } else {
                                    next_route
                                };
                                let action = set_switch_route(
                                    &model.base_url,
                                    model.timeout,
                                    &name,
                                    &route,
                                )?;
                                if let Some(state) = model.switches.get_mut(&name) {
                                    state.desired_route = route.clone();
                                    state.pending_route = Some(route);
                                    state.pending_until =
                                        Some(Instant::now() + Duration::from_secs(2));
                                }
                                model.apply_action_msg(action);
                                model.switch_confirm_active = false;
                                model.switch_confirm_start = None;
                            }
                        } else {
                            model.status = format!("switch {name} has no advertised routes");
                        }
                    }
                    ControlItem::RecoveryMode => {
                        model.recovery_mode = next_choice(&model.recovery_mode, RECOVERY_MODES);
                        model.status = format!("recovery mode={}", model.recovery_mode);
                    }
                    ControlItem::RecoveryRail => {
                        model.recovery_rail = next_choice(&model.recovery_rail, RECOVERY_RAILS);
                        model.status = format!("recovery rail={}", model.recovery_rail);
                    }
                    ControlItem::RecoveryEnter => {
                        if !model.recovery_confirm_active {
                            model.recovery_confirm_active = true;
                            model.recovery_confirm_start = Some(Instant::now());
                            model.status = format!(
                                "Press Enter again within 3s to confirm {} on {} (CON_MAS {}, target power cycle)",
                                recovery_mode_label(&model.recovery_mode),
                                model.recovery_rail,
                                recovery_active_level_label(&model.recovery_mode)
                            );
                        } else {
                            let action = enter_target_recovery(
                                &model.base_url,
                                model.timeout,
                                &model.recovery_mode,
                                &model.recovery_rail,
                            )?;
                            model.power_states.insert(model.recovery_rail.clone(), true);
                            model.apply_action_msg(action);
                            model.recovery_confirm_active = false;
                            model.recovery_confirm_start = None;
                            model.last_http_poll = Some(Instant::now());
                        }
                    }
                }
            }
        }
        (KeyCode::Char('g'), _) => {
            if let Some(idx) = first_gpio_index(model) {
                model.control_idx = idx;
            }
        }
        (KeyCode::Char('i'), _) => {
            if let Some(ControlItem::Gpio(gpio)) =
                control_items(model).get(model.control_idx).cloned()
            {
                let action = set_gpio_input(&model.base_url, model.timeout, &gpio)?;
                model.gpio_is_input.insert(gpio, true);
                model.apply_action_msg(action);
            }
        }
        (KeyCode::Char('u'), KeyModifiers::CONTROL)
        | (KeyCode::PageUp, _)
        | (KeyCode::Char('['), _) => {
            model.scroll_offset = model.scroll_offset.saturating_sub(3);
        }
        (KeyCode::PageDown, _)
        | (KeyCode::Char('d'), KeyModifiers::CONTROL)
        | (KeyCode::Char(']'), _) => {
            model.scroll_offset = model.scroll_offset.saturating_add(3);
        }
        _ => {}
    }
    Ok(false)
}

fn on_time_tick(model: &mut TuiModel) -> Result<()> {
    if let Some(result) = model.config_worker.poll() {
        let outcome = model.saved_config.finish(result);
        model.status = outcome.status().to_string();
    }
    if model.closed || model.paused {
        return Ok(());
    }

    if model.switch_confirm_active {
        if let Some(start) = model.switch_confirm_start {
            if start.elapsed() >= CONFIRM_TIMEOUT {
                model.switch_confirm_active = false;
                model.switch_confirm_start = None;
                model.status = "Switch confirmation timed out".to_string();
                model.last_http_poll = Some(Instant::now());
            }
        }
    }

    if model.recovery_confirm_active {
        if let Some(start) = model.recovery_confirm_start {
            if start.elapsed() >= CONFIRM_TIMEOUT {
                model.recovery_confirm_active = false;
                model.recovery_confirm_start = None;
                model.status = "Target recovery confirmation timed out".to_string();
                model.last_http_poll = Some(Instant::now());
            }
        }
    }

    let should_poll = match model.last_http_poll {
        Some(t) => t.elapsed() >= HTTP_POLL_INTERVAL,
        None => true,
    };
    if should_poll {
        poll_http(model)?;
    }
    Ok(())
}

fn poll_http(model: &mut TuiModel) -> Result<()> {
    model.last_http_poll = Some(Instant::now());
    let client = crate::client::BoardClient::new(&model.base_url, model.timeout)?;
    let status_data = client.send_text(BoardRequest {
        method: Method::GET,
        path: "/api/v1/status".to_string(),
        query: vec![],
        body: None,
    })?;
    let status_snapshot: WsStatusSnapshot = serde_json::from_str(&status_data)?;
    let config_changed = model.apply_status_snapshot(status_snapshot);

    let adc_data = client.send_text(BoardRequest {
        method: Method::GET,
        path: "/api/v1/adc/read".to_string(),
        query: vec![],
        body: None,
    })?;
    let adc_response = crate::adc::transform_response(&adc_data).map_err(|e| anyhow::anyhow!(e))?;
    model.apply_adc_response(adc_response.readings);

    if !model.switch_confirm_active && !model.recovery_confirm_active {
        model.status = "HTTP mode".to_string();
    }
    model.err = None;
    if config_changed {
        if let Some(request) = model.saved_config.request_refresh() {
            start_config_request(model, request);
        }
    }
    Ok(())
}

fn start_config_request(model: &mut TuiModel, request: ConfigRequest) {
    let kind = request.kind();
    if model
        .config_worker
        .start(model.base_url.clone(), model.timeout, request)
    {
        model.saved_config.start(kind);
        model.status = format!("Saved Config {}…", kind.as_str());
    }
}

fn perform_control_action(
    base_url: &str,
    timeout: Duration,
    output: &str,
    current_state: bool,
) -> Result<TuiActionMsg> {
    let next_state = if current_state { "off" } else { "on" };
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path: format!("/api/v1/power/{output}"),
        query: vec![],
        body: Some(serde_json::json!({ "state": next_state })),
    })?;
    Ok(TuiActionMsg {
        status: format!("power {output}={next_state}"),
        err: None,
    })
}

fn set_switch_route(
    base_url: &str,
    timeout: Duration,
    name: &str,
    route: &str,
) -> Result<TuiActionMsg> {
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path: format!("/api/v1/switch/{name}"),
        query: vec![],
        body: Some(serde_json::json!({ "route": route })),
    })?;
    Ok(TuiActionMsg {
        status: format!("switch {name}={route}"),
        err: None,
    })
}

fn set_gpio_output(
    base_url: &str,
    timeout: Duration,
    gpio: &str,
    value: bool,
) -> Result<TuiActionMsg> {
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path: format!("/api/v1/gpio/{gpio}"),
        query: vec![],
        body: Some(
            serde_json::json!({ "direction": "output", "value": if value { 1 } else { 0 } }),
        ),
    })?;
    Ok(TuiActionMsg {
        status: format!("gpio {}={}", gpio, if value { 1 } else { 0 }),
        err: None,
    })
}

fn set_gpio_input(base_url: &str, timeout: Duration, gpio: &str) -> Result<TuiActionMsg> {
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path: format!("/api/v1/gpio/{gpio}"),
        query: vec![],
        body: Some(serde_json::json!({ "direction": "input" })),
    })?;
    Ok(TuiActionMsg {
        status: format!("gpio {}=input", gpio),
        err: None,
    })
}

fn enter_target_recovery(
    base_url: &str,
    timeout: Duration,
    mode: &str,
    rail: &str,
) -> Result<TuiActionMsg> {
    if !RECOVERY_MODES.contains(&mode) {
        anyhow::bail!("unsupported target recovery mode {mode:?}");
    }
    if !RECOVERY_RAILS.contains(&rail) {
        anyhow::bail!("unsupported target recovery rail {rail:?}");
    }

    let client = crate::client::BoardClient::new(base_url, timeout.max(TARGET_RECOVERY_TIMEOUT))?;
    let response = client.send_text(BoardRequest {
        method: Method::POST,
        path: "/api/v1/target-recovery".to_string(),
        query: vec![],
        body: Some(serde_json::json!({ "mode": mode, "rail": rail })),
    })?;
    let response: serde_json::Value = serde_json::from_str(&response)?;
    if response.get("ok").and_then(serde_json::Value::as_bool) != Some(true)
        || response.get("mode").and_then(serde_json::Value::as_str) != Some(mode)
        || response.get("rail").and_then(serde_json::Value::as_str) != Some(rail)
        || response
            .get("release_direction")
            .and_then(serde_json::Value::as_str)
            != Some("input")
    {
        anyhow::bail!("target recovery response did not confirm the requested safe sequence");
    }

    Ok(TuiActionMsg {
        status: format!(
            "recovery {} on {} complete; CON_MAS released to input",
            recovery_mode_label(mode),
            rail
        ),
        err: None,
    })
}

fn next_choice(current: &str, choices: &[&str]) -> String {
    let next = choices
        .iter()
        .position(|choice| *choice == current)
        .map(|index| (index + 1) % choices.len())
        .unwrap_or(0);
    choices[next].to_string()
}

fn recovery_mode_label(mode: &str) -> &'static str {
    if mode == "qualcomm-edl" {
        "Qualcomm EDL"
    } else {
        "Rockchip MASKROM"
    }
}

fn recovery_mode_short_label(mode: &str) -> &'static str {
    if mode == "qualcomm-edl" {
        "QCOM EDL"
    } else {
        "RK MASKROM"
    }
}

fn recovery_active_level_label(mode: &str) -> &'static str {
    if mode == "qualcomm-edl" {
        "high"
    } else {
        "low"
    }
}

fn current_milliamp_estimate(reading: &AdcReading) -> i32 {
    match reading.kind {
        crate::adc::AdcKind::Current => {
            if let Some(ma_est) = reading.ma_est {
                return ma_est;
            }
            if let Some(current_ua) = reading.current_ua {
                return current_ua / 1000;
            }
            if let Some(sensor_value) = &reading.sensor_value {
                return (sensor_value.val1 * 1_000_000 + sensor_value.val2) / 1000;
            }
            0
        }
        crate::adc::AdcKind::Voltage => 0,
    }
}

fn control_targets() -> &'static [&'static str] {
    &["12v_out", "5v_out", "20v_out", "vdd_5v"]
}

fn next_switch_route(state: &TuiSwitchState) -> Option<String> {
    if state.routes.is_empty() {
        return None;
    }
    if let Some(index) = state
        .routes
        .iter()
        .position(|route| route == &state.desired_route)
    {
        return state.routes.get((index + 1) % state.routes.len()).cloned();
    }
    state.routes.first().cloned()
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum ControlItem {
    Power(String),
    Switch(String),
    RecoveryMode,
    RecoveryRail,
    RecoveryEnter,
    Gpio(String),
}

fn control_items(model: &TuiModel) -> Vec<ControlItem> {
    let mut items = Vec::new();
    for output in control_targets() {
        items.push(ControlItem::Power((*output).to_string()));
    }
    for name in model.switches.keys() {
        items.push(ControlItem::Switch(name.clone()));
    }
    items.push(ControlItem::RecoveryMode);
    items.push(ControlItem::RecoveryRail);
    items.push(ControlItem::RecoveryEnter);
    for gpio in &model.gpio_names {
        items.push(ControlItem::Gpio(gpio.clone()));
    }
    items
}

fn first_gpio_index(model: &TuiModel) -> Option<usize> {
    let count = control_targets().len() + model.switches.len() + 3;
    if model.gpio_names.is_empty() {
        None
    } else {
        Some(count)
    }
}

fn move_control_selection(model: &TuiModel, row_delta: isize, col_delta: isize) -> usize {
    let items = control_items(model);
    if items.is_empty() {
        return 0;
    }
    let content_width = if model.width > 0 { model.width } else { 80 };
    let rows = build_control_rows(model, content_width);
    let mut row_idx = 0usize;
    let mut col_idx = 0usize;

    'outer: for (r, row) in rows.iter().enumerate() {
        for (c, idx) in row.iter().enumerate() {
            if *idx == model.control_idx {
                row_idx = r;
                col_idx = c;
                break 'outer;
            }
        }
    }

    if row_delta != 0 {
        let next_row = (row_idx as isize + row_delta).clamp(0, rows.len() as isize - 1) as usize;
        let next_col = col_idx.min(rows[next_row].len().saturating_sub(1));
        return rows[next_row][next_col];
    }

    if col_delta != 0 {
        let next_col = col_idx as isize + col_delta;
        if next_col >= 0 && (next_col as usize) < rows[row_idx].len() {
            return rows[row_idx][next_col as usize];
        }
        if col_delta < 0 && row_idx > 0 {
            let prev_row = &rows[row_idx - 1];
            return prev_row[prev_row.len() - 1];
        }
        if col_delta > 0 && row_idx + 1 < rows.len() {
            return rows[row_idx + 1][0];
        }
    }

    model.control_idx
}

fn build_control_rows(model: &TuiModel, content_width: usize) -> Vec<Vec<usize>> {
    let (power, switches, recovery, gpio) = grouped_control_chips(model);
    let mut rows = Vec::new();
    rows.extend(build_section_navigation_rows(&power, content_width));
    rows.extend(build_section_navigation_rows(&switches, content_width));
    rows.extend(build_section_navigation_rows(&recovery, content_width));
    rows.extend(build_section_navigation_rows(&gpio, content_width));
    rows
}

fn build_control_chips(model: &TuiModel) -> Vec<String> {
    control_items(model)
        .into_iter()
        .map(|item| match item {
            ControlItem::Power(output) => {
                let state = if *model.power_states.get(&output).unwrap_or(&false) {
                    "on"
                } else {
                    "off"
                };
                format!("power {} [{}]", output, state)
            }
            ControlItem::Switch(name) => {
                let route = model
                    .switches
                    .get(&name)
                    .map(|state| state.desired_route.as_str())
                    .unwrap_or("");
                format!("switch {name} [{route}]")
            }
            ControlItem::RecoveryMode => {
                format!("mode [{}]", recovery_mode_short_label(&model.recovery_mode))
            }
            ControlItem::RecoveryRail => format!("rail [{}]", model.recovery_rail),
            ControlItem::RecoveryEnter => "enter recovery".to_string(),
            ControlItem::Gpio(gpio) => {
                let value = if *model.gpio_levels.get(&gpio).unwrap_or(&false) {
                    "1"
                } else {
                    "0"
                };
                let direction = if *model.gpio_is_input.get(&gpio).unwrap_or(&true) {
                    "in"
                } else {
                    "out"
                };
                let note = model.gpio_notes.get(&gpio).cloned().unwrap_or_default();
                format!("gpio {} [{}] {}={}", gpio, note, direction, value)
            }
        })
        .collect()
}

#[derive(Clone)]
struct RenderedControlChip {
    global_idx: usize,
    label: String,
}

fn grouped_control_chips(
    model: &TuiModel,
) -> (
    Vec<RenderedControlChip>,
    Vec<RenderedControlChip>,
    Vec<RenderedControlChip>,
    Vec<RenderedControlChip>,
) {
    let items = control_items(model);
    let labels = build_control_chips(model);
    let mut power = Vec::new();
    let mut switches = Vec::new();
    let mut recovery = Vec::new();
    let mut gpio = Vec::new();

    for (idx, item) in items.into_iter().enumerate() {
        let chip = RenderedControlChip {
            global_idx: idx,
            label: labels[idx].clone(),
        };
        match item {
            ControlItem::Power(_) => power.push(chip),
            ControlItem::Switch(_) => switches.push(chip),
            ControlItem::RecoveryMode | ControlItem::RecoveryRail | ControlItem::RecoveryEnter => {
                recovery.push(chip)
            }
            ControlItem::Gpio(_) => gpio.push(chip),
        }
    }

    (power, switches, recovery, gpio)
}

fn build_section_rows(chips: &[RenderedControlChip], content_width: usize) -> Vec<Vec<usize>> {
    let mut rows = Vec::new();
    let mut current_row = Vec::new();
    let mut current_width = 0usize;
    let cell_width = chip_cell_width(chips);

    for (idx, chip) in chips.iter().enumerate() {
        let _ = chip;
        let chip_width = cell_width;
        let mut candidate_width = current_width;
        if !current_row.is_empty() {
            candidate_width += 2;
        }
        candidate_width += chip_width;
        if candidate_width > content_width && !current_row.is_empty() {
            rows.push(current_row);
            current_row = vec![idx];
            current_width = chip_width;
            continue;
        }
        current_row.push(idx);
        current_width = candidate_width;
    }

    if !current_row.is_empty() {
        rows.push(current_row);
    }

    rows
}

fn build_section_navigation_rows(
    chips: &[RenderedControlChip],
    content_width: usize,
) -> Vec<Vec<usize>> {
    build_section_rows(chips, content_width)
        .into_iter()
        .map(|row| row.into_iter().map(|idx| chips[idx].global_idx).collect())
        .collect()
}

fn chip_cell_width(chips: &[RenderedControlChip]) -> usize {
    chips
        .iter()
        .map(|chip| chip.label.chars().count())
        .max()
        .unwrap_or(8)
        + 4
}

fn append_section_lines(
    lines: &mut Vec<Line<'static>>,
    title: &str,
    chips: &[RenderedControlChip],
    content_width: usize,
    selected_idx: usize,
) {
    lines.push(Line::from(Span::styled(
        title.to_string(),
        Style::default().add_modifier(Modifier::BOLD | Modifier::UNDERLINED),
    )));
    if chips.is_empty() {
        lines.push(Line::from("  (none)"));
        lines.push(Line::from(""));
        return;
    }

    let cell_width = chip_cell_width(chips);

    for row in build_section_rows(chips, content_width) {
        let mut spans = Vec::new();
        for idx in row {
            let chip = &chips[idx];
            let selected = chip.global_idx == selected_idx;
            let text = format!("{:^width$}", chip.label, width = cell_width);
            if selected {
                spans.push(Span::styled(
                    text,
                    Style::default()
                        .fg(Color::Black)
                        .bg(Color::White)
                        .add_modifier(Modifier::BOLD),
                ));
            } else {
                spans.push(Span::raw(text));
            }
            spans.push(Span::raw("  "));
        }
        lines.push(Line::from(spans));
    }
    lines.push(Line::from(""));
}

fn current_text_from_microamps(current_ua: i32) -> String {
    let (prefix, abs_ua) = if current_ua < 0 {
        ("-", -current_ua)
    } else {
        ("", current_ua)
    };
    format!("{}{:.6}A", prefix, abs_ua as f64 / 1_000_000.0)
}

fn render_ui(frame: &mut ratatui::Frame, model: &TuiModel) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(4),
            Constraint::Length(7),
            Constraint::Min(8),
        ])
        .split(frame.area());

    render_header(frame, chunks[0], model);
    render_sparklines(frame, chunks[1], model);
    render_body(frame, chunks[2], model);
    render_confirmation(frame, &model.saved_config);
}

fn render_header(frame: &mut ratatui::Frame, area: Rect, model: &TuiModel) {
    let block = Block::default().borders(Borders::ALL);
    let inner = block.inner(area);
    frame.render_widget(block, area);

    let header_chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(1), Constraint::Length(1)])
        .split(inner);

    let title = Paragraph::new("Radxa Linkr Debugger TUI")
        .alignment(Alignment::Center)
        .style(Style::default().add_modifier(Modifier::BOLD | Modifier::UNDERLINED));
    frame.render_widget(title, header_chunks[0]);

    let meta = Paragraph::new(format!(
        "Control Surface · url={} · status={} · keys: q quit · p pause · r refresh · arrows/Tab select item · Enter toggle · i selected GPIO to input · t/u sd route · [/]/PgUp/PgDn scroll",
        model.base_url, model.status
    ))
    .alignment(Alignment::Center);
    frame.render_widget(meta, header_chunks[1]);
}

fn render_sparklines(frame: &mut ratatui::Frame, area: Rect, model: &TuiModel) {
    let chunks = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Percentage(33),
            Constraint::Percentage(33),
            Constraint::Percentage(34),
        ])
        .split(area);

    for (idx, channel) in model.channel_ids.iter().take(3).enumerate() {
        let reading = model.latest.get(channel);
        let ma = reading.map(current_milliamp_estimate).unwrap_or(0);
        let power = reading
            .and_then(|reading| reading.power_enabled)
            .map(|enabled| if enabled { "on" } else { "off" })
            .unwrap_or("?");
        let current_text = reading
            .and_then(|reading| reading.current_ua)
            .map(current_text_from_microamps)
            .unwrap_or_else(|| format!("{:>5.2}A", ma as f64 / 1000.0));
        let max_points = chunks[idx].width.saturating_sub(2) as usize;
        let spark_data = model
            .history
            .get(channel)
            .map(|series| {
                let visible = if max_points > 0 && series.len() > max_points {
                    &series[series.len() - max_points..]
                } else {
                    series.as_slice()
                };
                visible
                    .iter()
                    .map(|value| (*value).max(0) as u64)
                    .collect::<Vec<_>>()
            })
            .unwrap_or_else(|| vec![0]);

        let spark = Sparkline::default()
            .block(
                Block::default()
                    .title(format!("{} {} {}", channel, power, current_text))
                    .borders(Borders::ALL),
            )
            .data(spark_data)
            .max(5000)
            .bar_set(symbols::bar::NINE_LEVELS);
        frame.render_widget(spark, chunks[idx]);
    }
}

fn render_body(frame: &mut ratatui::Frame, area: Rect, model: &TuiModel) {
    let section_width = area.width.saturating_sub(4) as usize;
    let (power, switches, recovery, gpio) = grouped_control_chips(model);
    let mut lines = vec![
        Line::from(Span::styled(
            "Controls",
            Style::default().add_modifier(Modifier::BOLD),
        )),
        Line::from(
            "  ↑/↓/←/→ select item   Enter/Space activate   i selected GPIO to input   g jump to first GPIO",
        ),
        Line::from(""),
    ];

    append_section_lines(
        &mut lines,
        "Power",
        &power,
        section_width,
        model.control_idx,
    );
    append_section_lines(
        &mut lines,
        "Switch",
        &switches,
        section_width,
        model.control_idx,
    );
    append_section_lines(
        &mut lines,
        "Target recovery",
        &recovery,
        section_width,
        model.control_idx,
    );
    append_section_lines(&mut lines, "GPIO", &gpio, section_width, model.control_idx);
    append_saved_config_lines(&mut lines, &model.saved_config, section_width);

    lines.push(Line::from(Span::styled(
        "Status",
        Style::default().add_modifier(Modifier::BOLD | Modifier::UNDERLINED),
    )));
    for state in model.switches.values() {
        lines.push(Line::from(format!(
            "  {} desired = {}{}",
            state.name,
            state.desired_route,
            if state.pending_route.is_some() {
                " (pending)"
            } else {
                ""
            }
        )));
        lines.push(Line::from(format!(
            "  {} actual  = {}",
            state.name, state.actual_route
        )));
    }
    lines.push(Line::from(format!(
        "  {}",
        format_monitoring_summary(&model.monitoring)
    )));

    if let Some(err) = &model.err {
        lines.push(Line::from(""));
        lines.push(Line::from(format!("error: {err}")));
    }

    lines.push(Line::from(""));
    lines.push(Line::from(
        "No-args starts the TUI. Existing command mode is unchanged when args are provided.",
    ));

    let content_height = lines.len();
    let paragraph = Paragraph::new(Text::from(lines))
        .block(Block::default().borders(Borders::ALL))
        .scroll((model.scroll_offset as u16, 0));
    frame.render_widget(paragraph, area);

    let scrollbar = Scrollbar::new(ScrollbarOrientation::VerticalRight);
    let mut state = ScrollbarState::default()
        .content_length(content_height)
        .position(model.scroll_offset);
    frame.render_stateful_widget(scrollbar, area, &mut state);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::client::DEFAULT_BASE_URL;
    use crate::ws_client::{TuiStatusGpio, TuiStatusSwitchInfo};
    use crossterm::event::KeyEventState;

    #[test]
    fn saved_config_starts_unsupported_without_firmware_summary() {
        let model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));

        assert!(!model.saved_config.is_supported());
    }

    #[test]
    fn apply_status_snapshot_updates_gpio_and_power_state() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.gpio_names = vec!["GP10".to_string()];
        model
            .gpio_notes
            .insert("GP10".to_string(), "J16_PIN1".to_string());
        model.gpio_levels.insert("GP10".to_string(), true);
        model.gpio_is_input.insert("GP10".to_string(), false);
        model.apply_status_snapshot(WsStatusSnapshot {
            power_outputs: vec![
                crate::ws_client::TuiStatusPowerOutput {
                    name: "5v_out".to_string(),
                    state: "on".to_string(),
                    value: 1,
                },
                crate::ws_client::TuiStatusPowerOutput {
                    name: "vdd_5v".to_string(),
                    state: "on".to_string(),
                    value: 1,
                },
            ],
            gpios: vec![
                TuiStatusGpio {
                    name: "GP11".to_string(),
                    pin: 11,
                    value: Some(0),
                    direction: "output".to_string(),
                    note: "J16_PIN2".to_string(),
                },
                TuiStatusGpio {
                    name: "GP7".to_string(),
                    pin: 7,
                    value: Some(0),
                    direction: "input".to_string(),
                    note: "CON_MAS".to_string(),
                },
            ],
            board_monitoring: BoardMonitoring::default(),
            ..Default::default()
        });

        // Snapshot becomes authoritative under REST polling.
        assert_eq!(model.power_states.get("5v_out"), Some(&true));
        assert_eq!(model.power_states.get("vdd_5v"), Some(&true));
        assert_eq!(model.gpio_levels.get("GP11"), Some(&false));
        assert_eq!(model.gpio_is_input.get("GP11"), Some(&false));
        assert_eq!(model.gpio_notes.get("GP11"), Some(&"J16_PIN2".to_string()));
        assert_eq!(model.gpio_levels.get("GP10"), None);
        assert!(!model.gpio_names.iter().any(|name| name == "GP7"));
        assert_eq!(model.gpio_notes.get("GP7"), None);
    }

    #[test]
    fn apply_adc_response_updates_history_and_power_state() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.apply_adc_response(vec![AdcReading {
            name: "5v_out".to_string(),
            signal: "S_C_5V".to_string(),
            raw: None,
            current_valid: Some(true),
            mv: Some(17),
            ma_est: Some(500),
            power_enabled: Some(true),
            kind: crate::adc::AdcKind::Current,
            sensor_channel: "current".to_string(),
            unit: "A".to_string(),
            value: Some(500000),
            sensor_value: None,
            current_ua: Some(500000),
            voltage_uv: None,
        }]);

        assert_eq!(model.power_states.get("5v_out"), Some(&true));
        assert_eq!(model.history.get("5v_out").map(|v| v.len()), Some(1));
    }

    #[test]
    fn milliamp_estimate_falls_back_to_current_microamps() {
        let reading = AdcReading {
            name: "5v_out".to_string(),
            signal: "S_C_5V".to_string(),
            raw: None,
            current_valid: None,
            mv: None,
            ma_est: None,
            power_enabled: Some(true),
            kind: crate::adc::AdcKind::Current,
            sensor_channel: "current".to_string(),
            unit: "A".to_string(),
            value: Some(850_000),
            sensor_value: None,
            current_ua: Some(850_000),
            voltage_uv: None,
        };

        assert_eq!(current_milliamp_estimate(&reading), 850);
    }

    #[test]
    fn voltage_reading_is_ignored_by_power_history_and_tui_sections() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.apply_adc_response(vec![AdcReading {
            name: "adc3".to_string(),
            signal: "ADC3".to_string(),
            raw: Some(42),
            current_valid: None,
            mv: Some(1234),
            ma_est: None,
            power_enabled: None,
            kind: crate::adc::AdcKind::Voltage,
            sensor_channel: "voltage".to_string(),
            unit: "V".to_string(),
            sensor_value: Some(crate::adc::AdcSensorValue {
                val1: 1,
                val2: 234000,
            }),
            value: Some(1_234_000),
            current_ua: None,
            voltage_uv: Some(1_234_000),
        }]);

        assert!(!model.history.contains_key("adc3"));
        assert!(!model.latest.contains_key("adc3"));
        assert!(!model.power_states.contains_key("adc3"));
    }

    #[test]
    fn time_tick_sets_last_http_poll_when_due() {
        let mut model = TuiModel::new("http://127.0.0.1:9".to_string(), Duration::from_secs(2));
        let _ = on_time_tick(&mut model);
        assert!(model.last_http_poll.is_some());
    }

    #[test]
    fn time_tick_does_nothing_when_paused() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.paused = true;
        on_time_tick(&mut model).unwrap();
        assert!(model.last_http_poll.is_none());
    }

    #[test]
    fn move_control_selection_right_advances() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.width = 80;
        assert_eq!(move_control_selection(&model, 0, 1), 1);
    }

    #[test]
    fn move_control_selection_left_stays_at_zero() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.width = 80;
        assert_eq!(move_control_selection(&model, 0, -1), 0);
    }

    #[test]
    fn build_control_rows_wraps_when_width_is_small() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.width = 24;
        let rows = build_control_rows(&model, 24);
        assert!(rows.len() >= 2, "rows={rows:?}");
    }

    #[test]
    fn move_control_selection_wraps_to_next_row() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.width = 24;
        model.control_idx = 1;
        assert_eq!(move_control_selection(&model, 0, 1), 2);
    }

    #[test]
    fn move_control_selection_down_reaches_switch_section() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.width = 120;
        let mut snapshot = WsStatusSnapshot::default();
        snapshot.switches.insert(
            "example".to_string(),
            TuiStatusSwitchInfo {
                route: "one".to_string(),
                routes: vec!["one".to_string(), "two".to_string()],
                ..Default::default()
            },
        );
        model.apply_status_snapshot(snapshot);
        model.control_idx = 0;
        assert_eq!(
            move_control_selection(&model, 1, 0),
            control_targets().len()
        );
    }

    #[test]
    fn g_jumps_to_first_gpio_in_unified_grid() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.gpio_names = vec!["GP13".to_string(), "GP14".to_string()];
        handle_key(
            &mut model,
            KeyEvent::new(KeyCode::Char('g'), KeyModifiers::NONE),
        )
        .unwrap();
        assert_eq!(
            model.control_idx,
            control_targets().len() + model.switches.len() + 3
        );
    }

    #[test]
    fn control_items_include_switches_and_recovery_between_power_and_gpio() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.gpio_names = vec!["GP13".to_string()];
        let mut snapshot = WsStatusSnapshot::default();
        snapshot.switches.insert(
            "alpha".to_string(),
            TuiStatusSwitchInfo {
                route: "one".to_string(),
                routes: vec!["one".to_string(), "two".to_string()],
                ..Default::default()
            },
        );
        snapshot.switches.insert(
            "beta".to_string(),
            TuiStatusSwitchInfo {
                route: "left".to_string(),
                routes: vec!["left".to_string(), "right".to_string()],
                ..Default::default()
            },
        );
        snapshot.gpios.push(TuiStatusGpio {
            name: "GP13".to_string(),
            pin: 13,
            direction: "input".to_string(),
            ..Default::default()
        });
        model.apply_status_snapshot(snapshot);

        let items = control_items(&model);
        assert_eq!(
            items[control_targets().len()],
            ControlItem::Switch("alpha".to_string())
        );
        assert_eq!(
            items[control_targets().len() + 1],
            ControlItem::Switch("beta".to_string())
        );
        assert_eq!(
            items[control_targets().len() + 2],
            ControlItem::RecoveryMode
        );
        assert_eq!(
            items[control_targets().len() + 3],
            ControlItem::RecoveryRail
        );
        assert_eq!(
            items[control_targets().len() + 4],
            ControlItem::RecoveryEnter
        );
        assert_eq!(
            items[control_targets().len() + 5],
            ControlItem::Gpio("GP13".to_string())
        );
    }

    #[test]
    fn switch_control_is_visible_only_when_reported_by_firmware() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        assert!(control_items(&model)
            .iter()
            .all(|item| !matches!(item, ControlItem::Switch(_))));

        let mut snapshot = WsStatusSnapshot::default();
        snapshot.switches.insert(
            "vin".to_string(),
            TuiStatusSwitchInfo {
                route: "3.3v".to_string(),
                routes: vec!["1.8v".to_string(), "3.3v".to_string()],
                ..Default::default()
            },
        );
        snapshot.gpios.push(TuiStatusGpio {
            name: "GP13".to_string(),
            pin: 13,
            direction: "input".to_string(),
            ..Default::default()
        });
        model.apply_status_snapshot(snapshot);

        let items = control_items(&model);
        assert_eq!(
            items[control_targets().len()],
            ControlItem::Switch("vin".to_string())
        );
        assert_eq!(first_gpio_index(&model), Some(control_targets().len() + 4));
        assert!(build_control_chips(&model)
            .iter()
            .any(|chip| chip == "switch vin [3.3v]"));
    }

    #[test]
    fn vin_control_uses_existing_confirmation_state() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        let mut snapshot = WsStatusSnapshot::default();
        snapshot.switches.insert(
            "vin".to_string(),
            TuiStatusSwitchInfo {
                route: "3.3v".to_string(),
                routes: vec!["3.3v".to_string(), "1.8v".to_string()],
                requires_confirm: true,
            },
        );
        model.apply_status_snapshot(snapshot);
        model.control_idx = control_targets().len();

        handle_key(
            &mut model,
            KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE),
        )
        .unwrap();

        assert!(model.switch_confirm_active);
        assert_eq!(model.switch_confirm_kind, "vin");
        assert_eq!(model.switch_confirm_target, "1.8v");
    }

    #[test]
    fn build_control_chips_show_current_switch_routes() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        let mut snapshot = WsStatusSnapshot::default();
        snapshot.switches.insert(
            "sd".to_string(),
            TuiStatusSwitchInfo {
                route: "usb-reader".to_string(),
                routes: vec!["target".to_string(), "usb-reader".to_string()],
                ..Default::default()
            },
        );
        snapshot.switches.insert(
            "usb".to_string(),
            TuiStatusSwitchInfo {
                route: "target".to_string(),
                routes: vec!["pc".to_string(), "target".to_string()],
                ..Default::default()
            },
        );
        model.apply_status_snapshot(snapshot);

        let chips = build_control_chips(&model);
        assert!(chips.iter().any(|chip| chip == "switch sd [usb-reader]"));
        assert!(chips.iter().any(|chip| chip == "switch usb [target]"));
    }

    #[test]
    fn recovery_controls_cycle_mode_and_rail_then_require_confirmation() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        let mut snapshot = WsStatusSnapshot::default();
        for name in ["sd", "usb"] {
            snapshot.switches.insert(
                name.to_string(),
                TuiStatusSwitchInfo {
                    route: "one".to_string(),
                    routes: vec!["one".to_string(), "two".to_string()],
                    ..Default::default()
                },
            );
        }
        model.apply_status_snapshot(snapshot);

        model.control_idx = control_targets().len() + 2;
        handle_key(
            &mut model,
            KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE),
        )
        .unwrap();
        assert_eq!(model.recovery_mode, "qualcomm-edl");

        model.control_idx = control_targets().len() + 3;
        handle_key(
            &mut model,
            KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE),
        )
        .unwrap();
        assert_eq!(model.recovery_rail, "12v_out");

        model.control_idx = control_targets().len() + 4;
        handle_key(
            &mut model,
            KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE),
        )
        .unwrap();
        assert!(model.recovery_confirm_active);
        assert!(model.status.contains("Qualcomm EDL"));
        assert!(model.status.contains("CON_MAS high"));

        handle_key(&mut model, KeyEvent::new(KeyCode::Esc, KeyModifiers::NONE)).unwrap();
        assert!(!model.recovery_confirm_active);
        assert_eq!(model.status, "Target recovery cancelled");
    }

    #[test]
    fn recovery_chip_labels_show_safe_sequence_choices() {
        let model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        let chips = build_control_chips(&model);
        let (_, _, recovery, _) = grouped_control_chips(&model);

        assert!(chips.iter().any(|chip| chip == "mode [RK MASKROM]"));
        assert!(chips.iter().any(|chip| chip == "rail [5v_out]"));
        assert!(chips.iter().any(|chip| chip == "enter recovery"));
        assert_eq!(build_section_rows(&recovery, 76).len(), 1);
    }

    #[test]
    fn apply_status_snapshot_tracks_actual_enumerated_switch_routes() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        let mut snapshot = WsStatusSnapshot::default();
        snapshot.switches.insert(
            "example".to_string(),
            TuiStatusSwitchInfo {
                route: "alternate".to_string(),
                routes: vec!["current".to_string(), "alternate".to_string()],
                ..Default::default()
            },
        );

        model.apply_status_snapshot(snapshot);

        let state = model.switches.get("example").unwrap();
        assert_eq!(state.actual_route, "alternate");
        assert_eq!(state.desired_route, "alternate");
    }

    #[test]
    fn apply_action_msg_updates_status() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.apply_action_msg(TuiActionMsg {
            status: "power 12v_out=on".to_string(),
            err: None,
        });
        assert_eq!(model.status, "power 12v_out=on");
    }

    #[test]
    fn gpio_selection_and_input_mode_update_status() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.gpio_names = vec!["GP13".to_string()];
        model.gpio_levels.insert("GP13".to_string(), false);
        model.gpio_is_input.insert("GP13".to_string(), false);
        model.control_idx = control_targets().len() + 3;

        model.apply_action_msg(TuiActionMsg {
            status: "gpio GP13=1".to_string(),
            err: None,
        });
        assert_eq!(model.status, "gpio GP13=1");

        model.apply_action_msg(TuiActionMsg {
            status: "gpio GP13=input".to_string(),
            err: None,
        });
        assert_eq!(model.status, "gpio GP13=input");
    }

    #[test]
    fn render_body_shows_unified_control_grid_and_hides_redundant_channel_lines() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        let mut snapshot = WsStatusSnapshot::default();
        snapshot.switches.insert(
            "sd".to_string(),
            TuiStatusSwitchInfo {
                route: "target".to_string(),
                routes: vec!["target".to_string(), "usb-reader".to_string()],
                ..Default::default()
            },
        );
        snapshot.switches.insert(
            "usb".to_string(),
            TuiStatusSwitchInfo {
                route: "pc".to_string(),
                routes: vec!["pc".to_string(), "target".to_string()],
                ..Default::default()
            },
        );
        snapshot.gpios.push(TuiStatusGpio {
            name: "GP10".to_string(),
            pin: 10,
            value: Some(1),
            direction: "output".to_string(),
            note: "J16_PIN1".to_string(),
        });
        model.apply_status_snapshot(snapshot);
        let mut lines = vec![
            Line::from("Controls"),
            Line::from("  ↑/↓/←/→ select item   Enter/Space toggle   i selected GPIO to input   g jump to first GPIO"),
            Line::from(""),
        ];
        let (power, switches, recovery, gpio) = grouped_control_chips(&model);
        append_section_lines(&mut lines, "Power", &power, 80, model.control_idx);
        append_section_lines(&mut lines, "Switch", &switches, 80, model.control_idx);
        append_section_lines(
            &mut lines,
            "Target recovery",
            &recovery,
            80,
            model.control_idx,
        );
        append_section_lines(&mut lines, "GPIO", &gpio, 80, model.control_idx);
        lines.push(Line::from(Span::styled(
            "Status",
            Style::default().add_modifier(Modifier::BOLD | Modifier::UNDERLINED),
        )));
        for state in model.switches.values() {
            lines.push(Line::from(format!(
                "  {} desired = {}",
                state.name, state.desired_route
            )));
            lines.push(Line::from(format!(
                "  {} actual  = {}",
                state.name, state.actual_route
            )));
        }
        let rendered = Text::from(lines).to_string();
        assert!(rendered.contains("Power"));
        assert!(rendered.contains("Switch"));
        assert!(rendered.contains("Target recovery"));
        assert!(rendered.contains("GPIO"));
        assert!(rendered.contains("gpio GP10 [J16_PIN1] out=1"));
        assert!(rendered.contains("power 12v_out [off]"));
        assert!(rendered.contains("switch sd [target]"));
        assert!(rendered.contains("switch usb [pc]"));
        assert!(rendered.contains("mode [RK MASKROM]"));
        assert!(rendered.contains("rail [5v_out]"));
        assert!(rendered.contains("sd desired = target"));
        assert!(rendered.contains("sd actual  = target"));
        assert!(rendered.contains("usb desired = pc"));
        assert!(rendered.contains("usb actual  = pc"));
        assert!(!rendered.contains("5v_out: 0mA power=off"));
    }

    #[test]
    fn quit_key_closes_tui() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        let quit = handle_key(
            &mut model,
            KeyEvent::new(KeyCode::Char('c'), KeyModifiers::CONTROL),
        )
        .unwrap();

        assert!(quit);
        assert!(model.closed);
    }

    #[test]
    fn non_press_key_events_are_ignored() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        let quit = handle_key(
            &mut model,
            KeyEvent {
                code: KeyCode::Char('c'),
                modifiers: KeyModifiers::CONTROL,
                kind: KeyEventKind::Release,
                state: KeyEventState::NONE,
            },
        )
        .unwrap();

        assert!(!quit);
        assert!(!model.closed);

        let before_idx = model.control_idx;
        handle_key(
            &mut model,
            KeyEvent {
                code: KeyCode::Right,
                modifiers: KeyModifiers::NONE,
                kind: KeyEventKind::Repeat,
                state: KeyEventState::NONE,
            },
        )
        .unwrap();

        assert_eq!(model.control_idx, before_idx);
    }
}
