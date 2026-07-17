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
use std::io::{self, Stdout};
use std::time::{Duration, Instant};

pub const TUI_HISTORY_LIMIT: usize = 240;
pub const TUI_POLL_INTERVAL: Duration = Duration::from_nanos(16_666_667);

#[derive(Debug)]
pub struct TuiActionMsg {
    pub status: String,
    pub err: Option<String>,
}

const HTTP_POLL_INTERVAL: Duration = Duration::from_secs(2);

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
    pub sd_route: String,
    pub usb_route: String,
    pub actual_sd_route: String,
    pub actual_usb_route: String,
    pub vin_available: bool,
    pub vin_route: String,
    pub actual_vin_route: String,
    pub pending_sd_route: Option<String>,
    pub pending_sd_route_until: Option<Instant>,
    pub pending_usb_route: Option<String>,
    pub pending_usb_route_until: Option<Instant>,
    pub pending_vin_route: Option<String>,
    pub pending_vin_route_until: Option<Instant>,
    pub switch_confirm_active: bool,
    pub switch_confirm_start: Option<Instant>,
    pub switch_confirm_kind: String,
    pub switch_confirm_target: String,
    pub gpio_names: Vec<String>,
    pub gpio_notes: std::collections::HashMap<String, String>,
    pub gpio_levels: std::collections::HashMap<String, bool>,
    pub gpio_is_input: std::collections::HashMap<String, bool>,
    pub monitoring: BoardMonitoring,
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
            sd_route: "target".to_string(),
            usb_route: "pc".to_string(),
            actual_sd_route: "target".to_string(),
            actual_usb_route: "pc".to_string(),
            vin_available: false,
            vin_route: "3.3v".to_string(),
            actual_vin_route: "3.3v".to_string(),
            pending_sd_route: None,
            pending_sd_route_until: None,
            pending_usb_route: None,
            pending_usb_route_until: None,
            pending_vin_route: None,
            pending_vin_route_until: None,
            switch_confirm_active: false,
            switch_confirm_start: None,
            switch_confirm_kind: String::new(),
            switch_confirm_target: String::new(),
            gpio_names: Vec::new(),
            gpio_notes: std::collections::HashMap::new(),
            gpio_levels: std::collections::HashMap::new(),
            gpio_is_input: std::collections::HashMap::new(),
            monitoring: BoardMonitoring::default(),
            closed: false,
            channel_ids: vec![
                "5v_out".to_string(),
                "12v_out".to_string(),
                "20v_out".to_string(),
            ],
        }
    }

    fn apply_status_snapshot(&mut self, snapshot: WsStatusSnapshot) {
        for output in snapshot.power_outputs {
            if output.name == "5v_ws" {
                continue;
            }
            self.power_states.insert(
                output.name.clone(),
                output.value != 0 || output.state == "on",
            );
        }
        if !snapshot.switches.sd.route.is_empty() {
            self.actual_sd_route = snapshot.switches.sd.route.clone();
            self.sd_route = snapshot.switches.sd.route;
            self.pending_sd_route = None;
            self.pending_sd_route_until = None;
        }
        if !snapshot.switches.usb.route.is_empty() {
            self.actual_usb_route = snapshot.switches.usb.route.clone();
            let now = Instant::now();
            match (&self.pending_usb_route, self.pending_usb_route_until) {
                (Some(pending), Some(_until)) if snapshot.switches.usb.route == *pending => {
                    self.usb_route = snapshot.switches.usb.route;
                    self.pending_usb_route = None;
                    self.pending_usb_route_until = None;
                }
                (Some(_), Some(until)) if now < until => {}
                _ => {
                    self.usb_route = snapshot.switches.usb.route;
                    self.pending_usb_route = None;
                    self.pending_usb_route_until = None;
                }
            }
        }
        self.vin_available = !snapshot.switches.vin.route.is_empty();
        if self.vin_available {
            self.actual_vin_route = snapshot.switches.vin.route.clone();
            let now = Instant::now();
            match (&self.pending_vin_route, self.pending_vin_route_until) {
                (Some(pending), Some(_)) if snapshot.switches.vin.route == *pending => {
                    self.vin_route = snapshot.switches.vin.route;
                    self.pending_vin_route = None;
                    self.pending_vin_route_until = None;
                }
                (Some(_), Some(until)) if now < until => {}
                _ => {
                    self.vin_route = snapshot.switches.vin.route;
                    self.pending_vin_route = None;
                    self.pending_vin_route_until = None;
                }
            }
        } else {
            self.pending_vin_route = None;
            self.pending_vin_route_until = None;
        }
        self.gpio_names = snapshot
            .gpios
            .iter()
            .map(|gpio| gpio.name.clone())
            .collect();
        self.gpio_notes.clear();
        self.gpio_levels.clear();
        self.gpio_is_input.clear();
        for gpio in &snapshot.gpios {
            self.gpio_notes.insert(gpio.name.clone(), gpio.note.clone());
            self.gpio_levels
                .insert(gpio.name.clone(), gpio.value.unwrap_or(0) != 0);
            self.gpio_is_input
                .insert(gpio.name.clone(), gpio.direction == "input");
        }
        self.monitoring = snapshot.board_monitoring;
    }

    fn apply_adc_response(&mut self, readings: Vec<AdcReading>) {
        for reading in readings {
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
        (KeyCode::Char('q'), _) | (KeyCode::Char('c'), KeyModifiers::CONTROL) => {
            model.closed = true;
            return Ok(true);
        }
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
                    ControlItem::SdSwitch => {
                        if !model.switch_confirm_active {
                            let next_route = if model.sd_route == "usb-reader" {
                                "target"
                            } else {
                                "usb-reader"
                            };
                            model.switch_confirm_active = true;
                            model.switch_confirm_start = Some(Instant::now());
                            model.switch_confirm_kind = "sd".to_string();
                            model.switch_confirm_target = next_route.to_string();
                            model.status = format!(
                                "Press Enter again within 3s to confirm SD switch to {}",
                                model.switch_confirm_target
                            );
                        } else if model.switch_confirm_kind == "sd" {
                            let route = model.switch_confirm_target.clone();
                            let action =
                                set_switch_route(&model.base_url, model.timeout, "sd", &route)?;
                            model.sd_route = route.clone();
                            model.pending_sd_route = Some(route);
                            model.pending_sd_route_until =
                                Some(Instant::now() + Duration::from_secs(2));
                            model.apply_action_msg(action);
                            model.switch_confirm_active = false;
                            model.switch_confirm_start = None;
                        }
                    }
                    ControlItem::UsbSwitch => {
                        if !model.switch_confirm_active {
                            let next_route = if model.usb_route == "target" {
                                "pc"
                            } else {
                                "target"
                            };
                            model.switch_confirm_active = true;
                            model.switch_confirm_start = Some(Instant::now());
                            model.switch_confirm_kind = "usb".to_string();
                            model.switch_confirm_target = next_route.to_string();
                            model.status = format!(
                                "Press Enter again within 3s to confirm USB switch to {}",
                                model.switch_confirm_target
                            );
                        } else if model.switch_confirm_kind == "usb" {
                            let route = model.switch_confirm_target.clone();
                            let action =
                                usb_switch_coordinated(&model.base_url, model.timeout, &route)?;
                            model.usb_route = route.clone();
                            model.pending_usb_route = Some(route);
                            model.pending_usb_route_until =
                                Some(Instant::now() + Duration::from_secs(2));
                            model.apply_action_msg(action);
                            model.switch_confirm_active = false;
                            model.switch_confirm_start = None;
                        }
                    }
                    ControlItem::VinSwitch => {
                        if !model.switch_confirm_active {
                            let next_route = if model.vin_route == "1.8v" {
                                "3.3v"
                            } else {
                                "1.8v"
                            };
                            model.switch_confirm_active = true;
                            model.switch_confirm_start = Some(Instant::now());
                            model.switch_confirm_kind = "vin".to_string();
                            model.switch_confirm_target = next_route.to_string();
                            model.status = format!(
                                "Press Enter again within 3s to confirm VIN switch to {}",
                                model.switch_confirm_target
                            );
                        } else if model.switch_confirm_kind == "vin" {
                            let route = model.switch_confirm_target.clone();
                            let action =
                                set_switch_route(&model.base_url, model.timeout, "vin", &route)?;
                            model.vin_route = route.clone();
                            model.pending_vin_route = Some(route);
                            model.pending_vin_route_until =
                                Some(Instant::now() + Duration::from_secs(2));
                            model.apply_action_msg(action);
                            model.switch_confirm_active = false;
                            model.switch_confirm_start = None;
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
        (KeyCode::Char('t'), _) => {
            let action = set_switch_route(&model.base_url, model.timeout, "sd", "target")?;
            model.sd_route = "target".to_string();
            model.pending_sd_route = Some("target".to_string());
            model.pending_sd_route_until = Some(Instant::now() + Duration::from_secs(2));
            model.apply_action_msg(action);
        }
        (KeyCode::Char('u'), KeyModifiers::CONTROL)
        | (KeyCode::PageUp, _)
        | (KeyCode::Char('['), _) => {
            model.scroll_offset = model.scroll_offset.saturating_sub(3);
        }
        (KeyCode::Char('u'), _) => {
            let action = set_switch_route(&model.base_url, model.timeout, "sd", "usb-reader")?;
            model.sd_route = "usb-reader".to_string();
            model.pending_sd_route = Some("usb-reader".to_string());
            model.pending_sd_route_until = Some(Instant::now() + Duration::from_secs(2));
            model.apply_action_msg(action);
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
    if model.closed || model.paused {
        return Ok(());
    }

    if model.switch_confirm_active {
        if let Some(start) = model.switch_confirm_start {
            if start.elapsed() >= Duration::from_secs(3) {
                model.switch_confirm_active = false;
                model.switch_confirm_start = None;
                model.status = "Switch confirmation timed out".to_string();
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
    model.apply_status_snapshot(status_snapshot);

    let adc_data = client.send_text(BoardRequest {
        method: Method::GET,
        path: "/api/v1/adc/read".to_string(),
        query: vec![],
        body: None,
    })?;
    let adc_response = crate::adc::transform_response(&adc_data).map_err(|e| anyhow::anyhow!(e))?;
    model.apply_adc_response(adc_response.readings);

    model.status = "HTTP mode".to_string();
    model.err = None;
    Ok(())
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

fn usb_switch_coordinated(base_url: &str, timeout: Duration, route: &str) -> Result<TuiActionMsg> {
    let client = crate::client::BoardClient::new(base_url, timeout)?;

    client.send_text(BoardRequest {
        method: Method::PUT,
        path: "/api/v1/switch/usb".to_string(),
        query: vec![],
        body: Some(serde_json::json!({ "route": route })),
    })?;

    Ok(TuiActionMsg {
        status: format!("switch usb={route}"),
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

fn current_milliamp_estimate(reading: &AdcReading) -> i32 {
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

fn control_targets() -> &'static [&'static str] {
    &["12v_out", "5v_out", "20v_out"]
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum ControlItem {
    Power(String),
    SdSwitch,
    UsbSwitch,
    VinSwitch,
    Gpio(String),
}

fn control_items(model: &TuiModel) -> Vec<ControlItem> {
    let mut items = Vec::new();
    for output in control_targets() {
        items.push(ControlItem::Power((*output).to_string()));
    }
    items.push(ControlItem::SdSwitch);
    items.push(ControlItem::UsbSwitch);
    if model.vin_available {
        items.push(ControlItem::VinSwitch);
    }
    for gpio in &model.gpio_names {
        items.push(ControlItem::Gpio(gpio.clone()));
    }
    items
}

fn first_gpio_index(model: &TuiModel) -> Option<usize> {
    let count = control_targets().len() + 2 + usize::from(model.vin_available);
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
    let (power, switches, gpio) = grouped_control_chips(model);
    let mut rows = Vec::new();
    rows.extend(build_section_navigation_rows(&power, content_width));
    rows.extend(build_section_navigation_rows(&switches, content_width));
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
            ControlItem::SdSwitch => format!("switch sd [{}]", model.sd_route),
            ControlItem::UsbSwitch => format!("switch usb [{}]", model.usb_route),
            ControlItem::VinSwitch => format!("switch vin [{}]", model.vin_route),
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
) {
    let items = control_items(model);
    let labels = build_control_chips(model);
    let mut power = Vec::new();
    let mut sd = Vec::new();
    let mut gpio = Vec::new();

    for (idx, item) in items.into_iter().enumerate() {
        let chip = RenderedControlChip {
            global_idx: idx,
            label: labels[idx].clone(),
        };
        match item {
            ControlItem::Power(_) => power.push(chip),
            ControlItem::SdSwitch | ControlItem::UsbSwitch | ControlItem::VinSwitch => {
                sd.push(chip)
            }
            ControlItem::Gpio(_) => gpio.push(chip),
        }
    }

    (power, sd, gpio)
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
    let (power, sd, gpio) = grouped_control_chips(model);
    let mut lines = vec![
        Line::from(Span::styled(
            "Controls",
            Style::default().add_modifier(Modifier::BOLD),
        )),
        Line::from(
            "  ↑/↓/←/→ select item   Enter/Space toggle   i selected GPIO to input   g jump to first GPIO   t target route   u usb-reader route",
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
    append_section_lines(&mut lines, "Switch", &sd, section_width, model.control_idx);
    append_section_lines(&mut lines, "GPIO", &gpio, section_width, model.control_idx);

    lines.push(Line::from(Span::styled(
        "Status",
        Style::default().add_modifier(Modifier::BOLD | Modifier::UNDERLINED),
    )));
    lines.push(Line::from(format!(
        "  sd desired = {}{}",
        model.sd_route,
        if model.pending_sd_route.is_some() {
            " (pending)"
        } else {
            ""
        }
    )));
    lines.push(Line::from(format!(
        "  sd actual  = {}",
        model.actual_sd_route
    )));
    lines.push(Line::from(format!(
        "  usb desired = {}{}",
        model.usb_route,
        if model.pending_usb_route.is_some() {
            " (pending)"
        } else {
            ""
        }
    )));
    lines.push(Line::from(format!(
        "  usb actual  = {}",
        model.actual_usb_route
    )));
    if model.vin_available {
        lines.push(Line::from(format!(
            "  vin desired = {}{}",
            model.vin_route,
            if model.pending_vin_route.is_some() {
                " (pending)"
            } else {
                ""
            }
        )));
        lines.push(Line::from(format!(
            "  vin actual  = {}",
            model.actual_vin_route
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
    use crate::ws_client::TuiStatusGpio;
    use crossterm::event::KeyEventState;

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
                    name: "5v_ws".to_string(),
                    state: "on".to_string(),
                    value: 1,
                },
            ],
            gpios: vec![TuiStatusGpio {
                name: "GP11".to_string(),
                pin: 11,
                value: Some(0),
                direction: "output".to_string(),
                note: "J16_PIN2".to_string(),
            }],
            board_monitoring: BoardMonitoring::default(),
            ..Default::default()
        });

        // Snapshot becomes authoritative under REST polling.
        assert_eq!(model.power_states.get("5v_out"), Some(&true));
        assert_eq!(model.power_states.get("5v_ws"), None);
        assert_eq!(model.gpio_levels.get("GP11"), Some(&false));
        assert_eq!(model.gpio_is_input.get("GP11"), Some(&false));
        assert_eq!(model.gpio_notes.get("GP11"), Some(&"J16_PIN2".to_string()));
        assert_eq!(model.gpio_levels.get("GP10"), None);
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
            sensor_channel: "current".to_string(),
            unit: "A".to_string(),
            sensor_value: None,
            current_ua: Some(500000),
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
            sensor_channel: "current".to_string(),
            unit: "A".to_string(),
            sensor_value: None,
            current_ua: Some(850_000),
        };

        assert_eq!(current_milliamp_estimate(&reading), 850);
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
        assert_eq!(model.control_idx, control_targets().len() + 2);
    }

    #[test]
    fn control_items_include_switches_between_power_and_gpio() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.gpio_names = vec!["GP13".to_string()];

        let items = control_items(&model);
        assert_eq!(items[control_targets().len()], ControlItem::SdSwitch);
        assert_eq!(items[control_targets().len() + 1], ControlItem::UsbSwitch);
        assert_eq!(
            items[control_targets().len() + 2],
            ControlItem::Gpio("GP13".to_string())
        );
    }

    #[test]
    fn vin_control_is_visible_only_when_reported_by_firmware() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        assert!(!control_items(&model).contains(&ControlItem::VinSwitch));

        let mut snapshot = WsStatusSnapshot::default();
        snapshot.switches.vin.route = "3.3v".to_string();
        snapshot.gpios.push(TuiStatusGpio {
            name: "GP13".to_string(),
            pin: 13,
            direction: "input".to_string(),
            ..Default::default()
        });
        model.apply_status_snapshot(snapshot);

        let items = control_items(&model);
        assert!(model.vin_available);
        assert_eq!(model.vin_route, "3.3v");
        assert_eq!(items[control_targets().len() + 2], ControlItem::VinSwitch);
        assert_eq!(first_gpio_index(&model), Some(control_targets().len() + 3));
        assert!(build_control_chips(&model)
            .iter()
            .any(|chip| chip == "switch vin [3.3v]"));
    }

    #[test]
    fn vin_control_uses_existing_confirmation_state() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.vin_available = true;
        model.vin_route = "3.3v".to_string();
        model.control_idx = control_targets().len() + 2;

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
        model.sd_route = "usb-reader".to_string();
        model.usb_route = "target".to_string();

        let chips = build_control_chips(&model);
        assert!(chips.iter().any(|chip| chip == "switch sd [usb-reader]"));
        assert!(chips.iter().any(|chip| chip == "switch usb [target]"));
    }

    #[test]
    fn apply_status_snapshot_tracks_actual_switch_routes() {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        let mut snapshot = WsStatusSnapshot::default();
        snapshot.switches.sd.route = "usb-reader".to_string();
        snapshot.switches.usb.route = "target".to_string();
        snapshot.switches.vin.route = "1.8v".to_string();

        model.apply_status_snapshot(snapshot);

        assert_eq!(model.actual_sd_route, "usb-reader");
        assert_eq!(model.actual_usb_route, "target");
        assert_eq!(model.sd_route, "usb-reader");
        assert_eq!(model.usb_route, "target");
        assert!(model.vin_available);
        assert_eq!(model.actual_vin_route, "1.8v");
        assert_eq!(model.vin_route, "1.8v");
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
        model.control_idx = control_targets().len() + 2;

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
        model.gpio_names = vec!["GP10".to_string()];
        model
            .gpio_notes
            .insert("GP10".to_string(), "J16_PIN1".to_string());
        model.gpio_levels.insert("GP10".to_string(), true);
        model.gpio_is_input.insert("GP10".to_string(), false);
        let mut lines = vec![
            Line::from("Controls"),
            Line::from("  ↑/↓/←/→ select item   Enter/Space toggle   i selected GPIO to input   g jump to first GPIO   t target route   u usb-reader route"),
            Line::from(""),
        ];
        let (power, sd, gpio) = grouped_control_chips(&model);
        append_section_lines(&mut lines, "Power", &power, 80, model.control_idx);
        append_section_lines(&mut lines, "Switch", &sd, 80, model.control_idx);
        append_section_lines(&mut lines, "GPIO", &gpio, 80, model.control_idx);
        lines.push(Line::from(Span::styled(
            "Status",
            Style::default().add_modifier(Modifier::BOLD | Modifier::UNDERLINED),
        )));
        lines.push(Line::from(format!("  sd desired = {}", model.sd_route)));
        lines.push(Line::from(format!(
            "  sd actual  = {}",
            model.actual_sd_route
        )));
        lines.push(Line::from(format!("  usb desired = {}", model.usb_route)));
        lines.push(Line::from(format!(
            "  usb actual  = {}",
            model.actual_usb_route
        )));
        let rendered = Text::from(lines).to_string();
        assert!(rendered.contains("Power"));
        assert!(rendered.contains("Switch"));
        assert!(rendered.contains("GPIO"));
        assert!(rendered.contains("gpio GP10 [J16_PIN1] out=1"));
        assert!(rendered.contains("power 12v_out [off]"));
        assert!(rendered.contains("switch sd [target]"));
        assert!(rendered.contains("switch usb [pc]"));
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
