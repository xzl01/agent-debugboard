use super::config_render::confirmation_text;
use super::config_rows::build_saved_config_content;
use super::config_state::ConfigConfirmation;
use super::confirm::{ConfirmableCommand, HardwareConfirmation};
use super::events_fixture;
use super::gpio_worker_fixture::ADC_EMPTY;
use super::mock_board::{mock_server, Reply};
use super::model::{TuiModel, TuiSwitchState};
use super::render::render_ui;
use super::runtime::poll_http;
use super::text_width::{clip_display, display_width};
use super::{config_state::SavedConfigState, gpio_fixture};
use crate::persistent_config::{
    ConfigAction, ConfigItemId, PersistentConfigResponse, PersistentConfigStatus,
};
use crate::ws_status::{TuiStatusSwitchInfo, WsStatusSnapshot};
use anyhow::{anyhow, Result};
use ratatui::backend::TestBackend;
use ratatui::buffer::Buffer;
use ratatui::Terminal;
use std::time::{Duration, Instant};

const CONFIG_MISSING_POWER: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":0,"items":[{"id":"switch/beta","kind":"switch","current":{"route":"pc"},"saved":{"route":"target"},"selected":false,"requires_confirm":false,"apply_state":"applied"}]}"#;
const CONFIG_POWER_SAFE: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":0,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":false,"apply_state":"applied"},{"id":"switch/beta","kind":"switch","current":{"route":"pc"},"saved":{"route":"target"},"selected":false,"requires_confirm":false,"apply_state":"applied"}]}"#;
const CONFIG_SWITCH_DANGEROUS: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":0,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":true,"apply_state":"applied"},{"id":"switch/beta","kind":"switch","current":{"route":"pc"},"saved":{"route":"target"},"selected":true,"requires_confirm":true,"apply_state":"applied"}]}"#;
const STATUS_ROUTE_REDUCED: &str = r#"{"switches":{"sd":{"route":"target","routes":["target"]}}}"#;

fn power_confirmation(output: &str) -> HardwareConfirmation {
    HardwareConfirmation::new(ConfirmableCommand::SetPower {
        output: output.to_string(),
        next_state: true,
    })
}

fn switch_confirmation(route: &str) -> HardwareConfirmation {
    HardwareConfirmation::new(ConfirmableCommand::RouteSwitch {
        name: "sd".to_string(),
        route: route.to_string(),
    })
}

fn config_state(raw: &str) -> Result<SavedConfigState> {
    let response = PersistentConfigResponse::from_raw(raw.to_string())?;
    response.validate(&ConfigAction::Get, None)?;
    let mut state = SavedConfigState::default();
    state.observe_summary(Some(PersistentConfigStatus {
        available: true,
        reason: "ready".to_string(),
        saved_count: 2,
        pending_count: 0,
    }));
    state
        .apply_authoritative(response)
        .map_err(anyhow::Error::msg)?;
    Ok(state)
}

fn draw(model: &mut TuiModel, width: u16, height: u16) -> Result<Buffer> {
    let backend = TestBackend::new(width, height);
    let mut terminal = Terminal::new(backend)?;
    terminal.draw(|frame| render_ui(frame, model))?;
    Ok(terminal.backend().buffer().clone())
}

fn buffer_text(buffer: &Buffer) -> String {
    let mut text = String::new();
    for y in 0..buffer.area.height {
        for x in 0..buffer.area.width {
            text.push_str(buffer[(x, y)].symbol());
        }
        text.push('\n');
    }
    text
}

#[test]
fn status_refresh_invalidates_power_confirmation_for_removed_output() {
    // Given a confirmation for an output present in the previous snapshot.
    let mut model = TuiModel::new("http://127.0.0.1:9".to_string(), Duration::from_secs(1));
    model.apply_status_snapshot(WsStatusSnapshot {
        power_outputs: gpio_fixture::current_power_outputs(),
        ..Default::default()
    });
    model.hardware_confirm = Some(power_confirmation("12v_out"));

    // When an authoritative snapshot removes that output.
    model.apply_status_snapshot(WsStatusSnapshot::default());

    // Then the stale confirmation is discarded.
    assert!(model.hardware_confirm.is_none());
}

#[test]
fn status_refresh_invalidates_switch_confirmation_for_unadvertised_route() {
    // Given a confirmation for a route advertised by the previous snapshot.
    let mut model = events_fixture::model_with_switch();
    model.hardware_confirm = Some(switch_confirmation("usb-reader"));
    let mut snapshot = WsStatusSnapshot::default();
    snapshot.switches.insert(
        "sd".to_string(),
        TuiStatusSwitchInfo {
            route: "target".to_string(),
            routes: vec!["target".to_string()],
            ..Default::default()
        },
    );

    // When the authoritative switch catalog no longer advertises that route.
    model.apply_status_snapshot(snapshot);

    // Then the stale confirmation is discarded.
    assert!(model.hardware_confirm.is_none());
}

#[test]
fn runtime_status_refresh_drops_stale_switch_confirm_before_any_put() -> Result<()> {
    // Given a board response that removes the route behind a pending modal.
    let (url, requests) = mock_server(vec![
        Reply::Http(200, STATUS_ROUTE_REDUCED),
        Reply::Http(200, ADC_EMPTY),
    ]);
    let mut model = events_fixture::model_with_switch();
    model.base_url = url;
    model.hardware_confirm = Some(switch_confirmation("usb-reader"));

    // When runtime polling applies that response and a confirm is attempted.
    poll_http(&mut model)?;

    // Then only the two authoritative GETs occur; no stale PUT is possible.
    assert!(model.hardware_confirm.is_none());
    assert!(requests
        .recv_timeout(Duration::from_secs(1))?
        .starts_with("GET /api/v1/status"));
    assert!(requests
        .recv_timeout(Duration::from_secs(1))?
        .starts_with("GET /api/v1/adc/read"));
    super::actions::confirm_hardware(&mut model, Instant::now())?;
    assert!(requests.recv_timeout(Duration::from_millis(30)).is_err());
    Ok(())
}

#[test]
fn saved_config_confirmation_invalidates_when_an_id_disappears() -> Result<()> {
    // Given a dangerous Save confirmation for two current firmware IDs.
    let mut state = config_state(super::keyboard_boundary_fixture::SHOW)?;
    state.focus();
    assert!(state.request_save().is_none());
    assert!(state.confirmation().is_some());

    let response = PersistentConfigResponse::from_raw(CONFIG_MISSING_POWER.to_string())?;
    response.validate(&ConfigAction::Get, None)?;
    // When an authoritative GET removes one of those IDs.
    state
        .apply_authoritative(response)
        .map_err(anyhow::Error::msg)?;

    // Then the pending confirmation cannot be submitted.
    assert!(state.confirmation().is_none());
    assert!(state.confirm().is_none());
    Ok(())
}

#[test]
fn saved_config_confirmation_invalidates_when_danger_semantics_change() -> Result<()> {
    // Given a confirmation covering both a dangerous and a safe item.
    for refreshed in [CONFIG_POWER_SAFE, CONFIG_SWITCH_DANGEROUS] {
        let mut state = config_state(super::keyboard_boundary_fixture::SHOW)?;
        state.focus();
        state.cursor = 1;
        state.toggle_current();
        assert!(state.request_save().is_none());
        assert!(state.confirmation().is_some());

        let response = PersistentConfigResponse::from_raw(refreshed.to_string())?;
        response.validate(&ConfigAction::Get, None)?;
        // When a refresh changes either item's danger semantics.
        state
            .apply_authoritative(response)
            .map_err(anyhow::Error::msg)?;

        // Then the old confirmation is invalid.
        assert!(state.confirmation().is_none());
    }
    Ok(())
}

#[test]
fn clip_display_removes_controls_before_width_clipping_and_keeps_cjk() {
    // Given printable CJK mixed with ESC, BEL, newline, and DEL.
    // When the text is clipped by display columns.
    let clipped = clip_display("A\u{1b}[31m\u{7}\n中\u{7f}", 8);

    // Then controls are gone while printable text and width accounting remain.
    assert_eq!(clipped, "A[31m中");
    assert_eq!(display_width(&clipped), 7);
    assert!(clipped.chars().all(|character| !character.is_control()));
}

#[test]
fn saved_config_dynamic_text_paths_strip_controls_and_keep_cjk() -> Result<()> {
    // Given hostile backend, error, and dangerous-ID display strings.
    let mut state = SavedConfigState::default();
    state.summary = Some(PersistentConfigStatus {
        available: false,
        reason: "原因".to_string(),
        saved_count: 1,
        pending_count: 0,
    });
    state.loaded = true;
    state.backend_available = false;
    state.backend_reason = "后端\u{1b}\n原因".to_string();
    state.error = Some("错误\u{7}\r".to_string());
    let dangerous = ConfigItemId("电源\u{1b}\n".to_string());
    state.confirmation = Some(ConfigConfirmation::Save {
        items: vec![dangerous.clone()],
        dangerous: vec![dangerous],
    });

    // When both body and modal text are produced.
    let body = build_saved_config_content(&state, 80)
        .lines
        .iter()
        .map(ToString::to_string)
        .collect::<String>();
    let modal = confirmation_text(&state).ok_or_else(|| anyhow!("confirmation must render"))?;

    // Then controls are absent and printable CJK survives both paths.
    assert!(body.chars().all(|character| !character.is_control()));
    assert!(modal.chars().all(|character| !character.is_control()));
    assert!(body.contains("后端") && modal.contains("电源"));
    Ok(())
}

#[test]
fn rendered_tui_buffer_contains_no_control_characters_from_dynamic_text() -> Result<()> {
    // Given hostile dynamic strings from URL, status, controls, GPIO, and modal state.
    let mut model = TuiModel::new(
        "http://设备/\u{1b}url\u{7}\n路径".to_string(),
        Duration::from_secs(1),
    );
    model.status = "状态\u{1b}[31m\n".to_string();
    model.err = Some("错误\u{7}\r".to_string());
    let power = "电源\u{1b}".to_string();
    model.power_names.push(power.clone());
    model.power_states.insert(power.clone(), true);
    let switch = "开关\n".to_string();
    model.switches.insert(
        switch.clone(),
        TuiSwitchState {
            name: switch,
            desired_route: "路线\u{1b}".to_string(),
            actual_route: "实际\n".to_string(),
            routes: vec!["广告\u{7}".to_string()],
            ..Default::default()
        },
    );
    let gpio = "GP\u{1b}13".to_string();
    model.gpio_names.push(gpio.clone());
    model.gpio_notes.insert(gpio, "注释\u{7}".to_string());
    model.hardware_confirm = Some(HardwareConfirmation {
        command: ConfirmableCommand::SetPower {
            output: power,
            next_state: false,
        },
        started: Instant::now(),
    });

    // When the complete TUI is rendered into a ratatui buffer.
    let buffer = draw(&mut model, 120, 20)?;
    // Then no cell contains a terminal control character and CJK remains visible.
    let text = buffer_text(&buffer);
    for y in 0..buffer.area.height {
        for x in 0..buffer.area.width {
            assert!(
                buffer[(x, y)]
                    .symbol()
                    .chars()
                    .all(|character| !character.is_control()),
                "control character at ({x},{y}): {:?}",
                buffer[(x, y)].symbol()
            );
        }
    }
    assert!(text.contains('设') || text.contains('电') || text.contains('注'));
    Ok(())
}
