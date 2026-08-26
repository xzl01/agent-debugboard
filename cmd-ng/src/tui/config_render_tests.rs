use super::config_render::render_confirmation;
use super::config_rows::build_saved_config_content;
use super::config_state::SavedConfigState;
use super::hit::HitRegions;
use crate::persistent_config::{ConfigAction, PersistentConfigResponse, PersistentConfigStatus};
use anyhow::{anyhow, Result};
use ratatui::backend::TestBackend;
use ratatui::layout::Rect;
use ratatui::style::{Color, Modifier};
use ratatui::text::Text;
use ratatui::Terminal;

const SHOW: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":1,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":true,"apply_state":"pending"}]}"#;

fn ready_state() -> Result<SavedConfigState> {
    let response =
        PersistentConfigResponse::from_raw(SHOW.to_string()).map_err(anyhow::Error::msg)?;
    response
        .validate(&ConfigAction::Get, None)
        .map_err(anyhow::Error::msg)?;
    let mut state = SavedConfigState::default();
    state.observe_summary(Some(PersistentConfigStatus {
        available: true,
        reason: "ready".to_string(),
        saved_count: 1,
        pending_count: 1,
    }));
    state
        .apply_authoritative(response)
        .map_err(anyhow::Error::msg)?;
    state.focus();
    Ok(state)
}

#[test]
fn rendered_row_exposes_firmware_values_selection_risk_apply_and_badges() -> Result<()> {
    let mut state = ready_state()?;
    state.error = Some("storage_error".to_string());
    let content = build_saved_config_content(&state, 120);
    let item_line = content
        .lines
        .iter()
        .map(ToString::to_string)
        .find(|line| line.contains("power/alpha"))
        .ok_or_else(|| anyhow!("saved config item row is missing"))?;
    let rendered = Text::from(content.lines).to_string();
    for token in [
        "power/alpha",
        "[x]",
        "power",
        "off",
        "on",
        "danger",
        "pending",
        "[pending:1]",
        "[error]",
        "error=storage_error",
    ] {
        assert!(
            rendered.contains(token),
            "missing token {token}: {rendered}"
        );
    }
    assert!(
        !item_line.contains("current="),
        "row must use bare column values, not key=value pairs: {item_line}"
    );
    assert_eq!(content.item_anchors.len(), 1);
    Ok(())
}

#[test]
fn wide_layout_keeps_not_saved_apply_state_unclipped() -> Result<()> {
    const SHOW_NOT_SAVED: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":0,"items":[{"id":"power/alpha","kind":"power","current":{"state":"on"},"saved":null,"selected":false,"requires_confirm":false,"apply_state":"not_saved"}]}"#;
    let response = PersistentConfigResponse::from_raw(SHOW_NOT_SAVED.to_string())
        .map_err(anyhow::Error::msg)?;
    response
        .validate(&ConfigAction::Get, None)
        .map_err(anyhow::Error::msg)?;
    let mut state = SavedConfigState::default();
    state.observe_summary(Some(PersistentConfigStatus {
        available: true,
        reason: "ready".to_string(),
        saved_count: 0,
        pending_count: 0,
    }));
    state
        .apply_authoritative(response)
        .map_err(anyhow::Error::msg)?;

    let content = build_saved_config_content(&state, 120);
    let item_line = content
        .lines
        .iter()
        .map(ToString::to_string)
        .find(|line| line.contains("power/alpha"))
        .ok_or_else(|| anyhow!("saved config item row is missing"))?;
    assert!(
        item_line.contains("not_saved"),
        "APPLY column must render the full not_saved token at 120 columns: {item_line}"
    );
    Ok(())
}

#[test]
fn old_firmware_renders_unsupported_without_items() {
    let state = SavedConfigState::default();
    let rendered = Text::from(build_saved_config_content(&state, 32).lines).to_string();
    assert!(rendered.contains("[unsupported]"));
    assert!(!rendered.contains("power/"));
}

#[test]
fn first_refresh_disconnect_renders_error_detail_instead_of_loading() {
    let mut state = SavedConfigState::default();
    state.observe_summary(Some(PersistentConfigStatus {
        available: true,
        reason: "ready".to_string(),
        saved_count: 0,
        pending_count: 0,
    }));
    state.error = Some("disconnect".to_string());
    let rendered = Text::from(build_saved_config_content(&state, 48).lines).to_string();
    assert!(rendered.contains("[error]"));
    assert!(rendered.contains("error=disconnect"));
    assert!(!rendered.contains("[loading]"));
    assert!(!rendered.contains("[unavailable]"));
}

fn confirmation_area(frame_area: Rect) -> Rect {
    let width = frame_area.width.saturating_sub(2).clamp(1, 72);
    let height = frame_area.height.saturating_sub(2).clamp(1, 7);
    Rect::new(
        frame_area.x + (frame_area.width - width) / 2,
        frame_area.y + (frame_area.height - height) / 2,
        width,
        height,
    )
}

#[test]
fn save_confirmation_uses_red_border_and_yellow_bold_emphasis() -> Result<()> {
    for (width, height) in [(80u16, 24u16), (120, 32)] {
        let mut state = ready_state()?;
        assert!(state.request_save().is_none());
        let backend = TestBackend::new(width, height);
        let mut terminal = Terminal::new(backend)?;
        let mut hits = HitRegions::default();
        terminal.draw(|frame| render_confirmation(frame, &state, &mut hits))?;
        let buffer = terminal.backend().buffer().clone();
        let area = confirmation_area(Rect::new(0, 0, width, height));

        let corner = &buffer[(area.x, area.y)];
        assert_eq!(
            corner.fg,
            Color::Red,
            "modal border must be red at {width}x{height}: corner={corner:?}"
        );

        let title = &buffer[(area.x + 1, area.y)];
        assert_eq!(title.symbol(), "S", "title cell missing: {title:?}");
        assert_eq!(
            title.fg,
            Color::Yellow,
            "modal title must use modal.emphasis: {title:?}"
        );
        assert!(title.modifier.contains(Modifier::BOLD));

        let message = &buffer[(area.x + 1, area.y + 1)];
        assert_eq!(message.symbol(), "S", "danger message missing: {message:?}");
        assert_eq!(
            message.fg,
            Color::Yellow,
            "danger message must use modal.emphasis: {message:?}"
        );
        assert!(message.modifier.contains(Modifier::BOLD));
    }
    Ok(())
}

#[test]
fn compact_terminal_clips_confirmation_without_panicking() -> Result<()> {
    let mut state = ready_state()?;
    assert!(state.request_save().is_none());
    let backend = TestBackend::new(20, 7);
    let mut terminal = Terminal::new(backend)?;

    let mut hits = HitRegions::default();
    terminal.draw(|frame| render_confirmation(frame, &state, &mut hits))?;

    assert_eq!(terminal.backend().buffer().area.width, 20);
    assert_eq!(terminal.backend().buffer().area.height, 7);
    Ok(())
}

#[test]
fn save_confirmation_names_the_dangerous_firmware_ids() -> Result<()> {
    let mut save = ready_state()?;
    assert!(save.request_save().is_none());
    let save_text = super::config_render::confirmation_text(&save)
        .ok_or_else(|| anyhow!("saved config confirmation text is missing"))?;

    assert!(save_text.contains("SAVE"));
    assert!(save_text.contains("power/alpha"));
    Ok(())
}
