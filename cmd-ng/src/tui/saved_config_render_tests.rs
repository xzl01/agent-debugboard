use super::model::TuiModel;
use super::pages::ActivePage;
use super::render::render_ui;
use crate::client::DEFAULT_BASE_URL;
use crate::persistent_config::{ConfigAction, PersistentConfigResponse, PersistentConfigStatus};
use anyhow::Result;
use ratatui::backend::TestBackend;
use ratatui::layout::Rect;
use ratatui::Terminal;
use std::time::Duration;

const SHOW: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":1,"items":[{"id":"power/0","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":true,"apply_state":"pending"},{"id":"power/1","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":false,"requires_confirm":false,"apply_state":"applied"},{"id":"power/2","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":false,"requires_confirm":false,"apply_state":"applied"},{"id":"power/3","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":false,"requires_confirm":false,"apply_state":"applied"}]}"#;

fn loaded_model() -> Result<TuiModel> {
    let response =
        PersistentConfigResponse::from_raw(SHOW.to_string()).map_err(anyhow::Error::msg)?;
    response
        .validate(&ConfigAction::Get, None)
        .map_err(anyhow::Error::msg)?;
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model
        .saved_config
        .observe_summary(Some(PersistentConfigStatus {
            available: true,
            reason: "ready".to_string(),
            saved_count: 1,
            pending_count: 1,
        }));
    model
        .saved_config
        .apply_authoritative(response)
        .map_err(anyhow::Error::msg)?;
    model.set_page(ActivePage::SavedConfig);
    Ok(model)
}

fn draw(model: &mut TuiModel, width: u16, height: u16) -> Result<String> {
    let backend = TestBackend::new(width, height);
    let mut terminal = Terminal::new(backend)?;
    terminal.draw(|frame| render_ui(frame, model))?;
    Ok(terminal
        .backend()
        .buffer()
        .content()
        .iter()
        .map(|cell| cell.symbol())
        .collect())
}

#[test]
fn visible_items_register_full_width_typed_hits_after_scroll() -> Result<()> {
    let mut model = loaded_model()?;
    model.saved_config.blur();
    model.config_scroll = 2;

    draw(&mut model, 40, 8)?;

    let hits = model.hit_map.saved_config_rows.iter().collect::<Vec<_>>();
    assert_eq!(hits.len(), 3);
    assert_eq!(
        hits.iter().map(|(rect, _)| **rect).collect::<Vec<_>>(),
        [
            Rect::new(0, 4, 40, 1),
            Rect::new(0, 5, 40, 1),
            Rect::new(0, 6, 40, 1),
        ]
    );
    assert_eq!(
        hits.iter()
            .map(|(_, target)| target.0.as_str())
            .collect::<Vec<_>>(),
        ["power/1", "power/2", "power/3"]
    );
    Ok(())
}

#[test]
fn non_item_saved_config_states_never_register_row_hits() -> Result<()> {
    let mut unsupported = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    unsupported.set_page(ActivePage::SavedConfig);
    let mut loading = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    loading
        .saved_config
        .observe_summary(Some(PersistentConfigStatus {
            available: true,
            reason: "ready".to_string(),
            saved_count: 0,
            pending_count: 0,
        }));
    loading.saved_config.error = Some("disconnect".to_string());
    loading.set_page(ActivePage::SavedConfig);
    let mut unavailable = loaded_model()?;
    unavailable.saved_config.backend_available = false;
    let mut empty = loaded_model()?;
    empty.saved_config.items.clear();

    for model in [&mut unsupported, &mut loading, &mut unavailable, &mut empty] {
        draw(model, 40, 8)?;
        assert!(model.hit_map.saved_config_rows.is_empty());
    }
    Ok(())
}

#[test]
fn confirmation_renders_complete_button_hits_only_when_they_fit() -> Result<()> {
    for (width, expected_hits) in [(80, 2), (20, 0)] {
        let mut model = loaded_model()?;
        assert!(model.saved_config.request_save().is_none());

        let rendered = draw(&mut model, width, 8)?;

        assert_eq!(
            model.hit_map.saved_config_modal.iter().count(),
            expected_hits
        );
        assert_eq!(rendered.contains("[ Confirm ]"), expected_hits == 2);
        assert_eq!(rendered.contains("[ Cancel ]"), expected_hits == 2);
    }
    Ok(())
}
