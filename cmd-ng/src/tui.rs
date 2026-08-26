// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::client::BoardTransport;
use anyhow::Result;
use ratatui::{backend::CrosstermBackend, Terminal};
use std::io;
use std::time::Duration;

mod actions;
#[cfg(test)]
mod actions_tests;
mod board_io;
#[cfg(test)]
mod board_io_tests;
#[cfg(test)]
mod cjk_render_tests;
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
#[cfg(test)]
mod config_worker_tests;
mod confirm;
#[cfg(test)]
mod confirm_tests;
#[cfg(test)]
mod confirmation_event_deadline_tests;
mod control_columns;
mod control_rows;
#[cfg(test)]
mod control_rows_tests;
mod controls;
#[cfg(test)]
mod controls_tests;
#[cfg(test)]
mod dense_layout_tests;
mod direct_gpio_key;
mod events;
#[cfg(test)]
mod events_fixture;
#[cfg(test)]
mod events_tests;
#[cfg(test)]
mod gpio_direct_key_modifier_tests;
#[cfg(test)]
mod gpio_fixture;
mod gpio_gesture;
mod gpio_gesture_control;
#[cfg(test)]
mod gpio_gesture_runtime_tests;
#[cfg(test)]
mod gpio_gesture_tests;
mod gpio_gesture_types;
#[cfg(test)]
mod gpio_hit_tests;
#[cfg(test)]
mod gpio_hold_render_tests;
mod gpio_io;
#[cfg(test)]
mod gpio_io_tests;
#[cfg(test)]
mod gpio_keybar_tests;
#[cfg(test)]
mod gpio_keyboard_tests;
mod gpio_layout;
#[cfg(test)]
mod gpio_layout_tests;
mod gpio_mouse_events;
#[cfg(test)]
mod gpio_mouse_gesture_tests;
#[cfg(test)]
mod gpio_mouse_tests;
mod gpio_projection;
#[cfg(test)]
mod gpio_projection_tests;
#[cfg(test)]
mod gpio_render_lifecycle_tests;
#[cfg(test)]
mod gpio_render_tests;
#[cfg(test)]
mod gpio_worker_fixture;
#[cfg(test)]
mod gpio_worker_loop_tests;
mod hit;
#[cfg(test)]
mod hit_tests;
mod hit_types;
#[cfg(test)]
mod keyboard_boundary_fixture;
#[cfg(test)]
mod keyboard_redraw_boundary_tests;
#[cfg(test)]
mod loop_tests;
#[cfg(test)]
mod mock_board;
mod model;
#[cfg(test)]
mod model_tests;
mod mouse_events;
#[cfg(test)]
mod mouse_events_tests;
#[cfg(test)]
mod mouse_fixture;
#[cfg(test)]
mod mouse_input_safety_tests;
#[cfg(test)]
mod mouse_outcome_tests;
#[cfg(test)]
mod mouse_page_pass_through_tests;
#[cfg(test)]
mod mouse_page_tests;
#[cfg(test)]
mod mouse_redraw_boundary_tests;
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
mod render_body_hits;
#[cfg(test)]
mod render_chrome_tests;
mod render_keybar;
mod render_modal;
#[cfg(test)]
mod render_modal_tests;
#[cfg(test)]
mod render_selection_tests;
mod render_status;
mod render_telemetry;
#[cfg(test)]
mod render_telemetry_tests;
#[cfg(test)]
mod render_tests;
#[cfg(test)]
mod resize_redraw_boundary_tests;
mod runtime;
#[cfg(test)]
mod safety_input_tests;
#[cfg(test)]
mod safety_refresh_tests;
#[cfg(test)]
mod saved_config_error_key_tests;
#[cfg(test)]
mod saved_config_gpio_gesture_tests;
#[cfg(test)]
mod saved_config_mouse_tests;
#[cfg(test)]
mod saved_config_render_tests;
mod status_page;
#[cfg(test)]
mod status_page_tests;
#[cfg(test)]
mod switch_mouse_tests;
#[cfg(test)]
mod switch_render_tests;
mod terminal;
#[cfg(test)]
mod terminal_tests;
mod text_width;

#[cfg(test)]
use events::handle_key;
use model::TuiModel;
use terminal::{finish, TerminalSession};

pub const TUI_HISTORY_LIMIT: usize = 240;
pub const TUI_POLL_INTERVAL: Duration = Duration::from_nanos(16_666_667);

#[derive(Debug)]
pub struct TuiActionMsg {
    pub status: String,
    pub err: Option<String>,
}

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

    let result = runtime::event_loop(&mut model, &mut terminal);

    finish(result, &mut session)
}
