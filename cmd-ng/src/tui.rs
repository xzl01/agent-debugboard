// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::client::{BoardRequest, BoardTransport};
use crate::ws_status::WsStatusSnapshot;
use anyhow::Result;
use crossterm::event::{self, Event};
use ratatui::backend::CrosstermBackend;
use ratatui::Terminal;
use reqwest::Method;
use std::io::{self, Stdout};
use std::time::{Duration, Instant};

mod actions;
#[cfg(test)]
mod actions_tests;
mod board_io;
mod config_columns;
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
mod config_rows;
mod config_state;
#[cfg(test)]
mod config_state_tests;
mod confirm;
#[cfg(test)]
mod confirm_tests;
mod control_columns;
mod control_rows;
#[cfg(test)]
mod control_rows_tests;
mod controls;
#[cfg(test)]
mod controls_tests;
#[cfg(test)]
mod dense_layout_tests;
mod events;
#[cfg(test)]
mod events_tests;
#[cfg(test)]
mod gpio_fixture;
#[cfg(test)]
mod gpio_hit_tests;
#[cfg(test)]
mod gpio_layout_tests;
mod gpio_projection;
#[cfg(test)]
mod gpio_projection_tests;
mod hit;
#[cfg(test)]
mod loop_tests;
mod model;
#[cfg(test)]
mod model_tests;
#[cfg(test)]
mod mouse_tests;
#[cfg(test)]
mod navigation_tests;
mod page_header;
#[cfg(test)]
mod page_tabs_tests;
mod pages;
#[cfg(test)]
mod pages_tests;
#[cfg(test)]
mod projection_navigation_tests;
mod render;
mod render_body;
#[cfg(test)]
mod render_chrome_tests;
mod render_keybar;
mod render_modal;
#[cfg(test)]
mod render_modal_tests;
mod render_status;
mod render_telemetry;
#[cfg(test)]
mod render_telemetry_tests;
#[cfg(test)]
mod render_tests;
mod status_page;
#[cfg(test)]
mod status_page_tests;
mod terminal;
#[cfg(test)]
mod terminal_tests;

use config_state::ConfigRequest;
use events::{handle_key, handle_mouse};
use model::TuiModel;
use render::render_ui;
use terminal::{finish, TerminalSession};

pub const TUI_HISTORY_LIMIT: usize = 240;
pub const TUI_POLL_INTERVAL: Duration = Duration::from_nanos(16_666_667);

#[derive(Debug)]
pub struct TuiActionMsg {
    pub status: String,
    pub err: Option<String>,
}

const HTTP_POLL_INTERVAL: Duration = Duration::from_secs(2);

pub fn run_tui<TClient>(client: TClient, base_url: String, timeout: Duration) -> Result<u8>
where
    TClient: BoardTransport,
{
    let _ = client.base_url();
    let mut model = TuiModel::new(base_url, timeout);

    let mut session = TerminalSession::enter()?;
    let stdout = io::stdout();
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;

    let result = event_loop(&mut model, &mut terminal);

    finish(result, &mut session)
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
            match event::read()? {
                Event::Key(key) if handle_key(model, key)? => {
                    return Ok(0);
                }
                Event::Key(_) => {}
                Event::Mouse(mouse) => handle_mouse(model, mouse)?,
                _ => {}
            }
        }
    }
}

fn on_time_tick(model: &mut TuiModel) -> Result<()> {
    if let Some(result) = model.config_worker.poll() {
        let outcome = model.saved_config.finish(result);
        model.status = outcome.status().to_string();
    }

    if model
        .hardware_confirm
        .as_ref()
        .is_some_and(|confirm| confirm.expired())
    {
        if let Some(confirm) = model.hardware_confirm.take() {
            model.status = confirm.command.timeout_message();
            model.last_http_poll = Some(Instant::now());
        }
    }

    if model.closed || model.paused {
        return Ok(());
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

    if model.hardware_confirm.is_none() {
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
