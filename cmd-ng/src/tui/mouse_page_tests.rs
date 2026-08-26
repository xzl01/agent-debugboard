use super::config_state::ConfigConfirmation;
use super::gpio_gesture::GpioGestureInput;
use super::hit_types::TabTarget;
use super::model::TuiModel;
use super::mouse_events::handle_mouse_at;
use super::mouse_fixture::{draw_sized, model, power_confirmation};
use super::pages::ActivePage;
use anyhow::{anyhow, Result};
use crossterm::event::{KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use ratatui::layout::Rect;
use std::time::Instant;

#[derive(Debug, Clone, Copy)]
enum BlockingState {
    SavedConfigConfirmation,
    SavedConfigError,
    HardwareConfirmation,
}

fn expected_hits(active: ActivePage, row: u16) -> [(Rect, TabTarget); 3] {
    match active {
        ActivePage::Controls => [
            (Rect::new(0, row, 10, 1), TabTarget(ActivePage::Controls)),
            (
                Rect::new(12, row, 12, 1),
                TabTarget(ActivePage::SavedConfig),
            ),
            (Rect::new(26, row, 6, 1), TabTarget(ActivePage::Status)),
        ],
        ActivePage::SavedConfig => [
            (Rect::new(0, row, 8, 1), TabTarget(ActivePage::Controls)),
            (
                Rect::new(10, row, 14, 1),
                TabTarget(ActivePage::SavedConfig),
            ),
            (Rect::new(26, row, 6, 1), TabTarget(ActivePage::Status)),
        ],
        ActivePage::Status => [
            (Rect::new(0, row, 8, 1), TabTarget(ActivePage::Controls)),
            (
                Rect::new(10, row, 12, 1),
                TabTarget(ActivePage::SavedConfig),
            ),
            (Rect::new(24, row, 8, 1), TabTarget(ActivePage::Status)),
        ],
    }
}

fn tab_rect(model: &TuiModel, page: ActivePage) -> Result<Rect> {
    model
        .hit_map
        .tabs
        .iter()
        .find(|(_, target)| **target == TabTarget(page))
        .map(|(rect, _)| *rect)
        .ok_or_else(|| anyhow!("missing {page:?} tab hit"))
}

fn mouse(kind: MouseEventKind, column: u16, row: u16) -> MouseEvent {
    MouseEvent {
        kind,
        column,
        row,
        modifiers: KeyModifiers::NONE,
    }
}

fn seed_gesture(model: &mut TuiModel, now: Instant) {
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP10"),
            column: 40,
            row: 10,
        },
        now,
    );
}

#[test]
fn rendered_tab_hits_match_visible_spans_at_supported_widths() -> Result<()> {
    for (width, height) in [(47, 24), (48, 24), (80, 24), (120, 32)] {
        for page in [
            ActivePage::Controls,
            ActivePage::SavedConfig,
            ActivePage::Status,
        ] {
            // Given a page rendered without scope telemetry.
            let mut model = model();
            model.set_page(page);

            // When the complete frame is drawn.
            draw_sized(&mut model, width, height)?;

            // Then each hit is exactly its visible label span on row three.
            let actual = model
                .hit_map
                .tabs
                .iter()
                .map(|(rect, target)| (*rect, *target))
                .collect::<Vec<_>>();
            assert_eq!(
                actual,
                expected_hits(page, 2),
                "width={width} page={page:?}"
            );
        }
    }
    Ok(())
}

#[test]
fn left_down_on_each_inactive_tab_switches_pages_and_cancels_gestures() -> Result<()> {
    for (start_page, target_page) in [
        (ActivePage::SavedConfig, ActivePage::Controls),
        (ActivePage::Controls, ActivePage::SavedConfig),
        (ActivePage::Controls, ActivePage::Status),
    ] {
        // Given an inactive rendered tab and an active GPIO gesture.
        let now = Instant::now();
        let mut model = model();
        model.set_page(start_page);
        draw_sized(&mut model, 80, 24)?;
        let rect = tab_rect(&model, target_page)?;
        seed_gesture(&mut model, now);

        // When its visible span receives left-button Down.
        handle_mouse_at(
            &mut model,
            mouse(MouseEventKind::Down(MouseButton::Left), rect.x, rect.y),
            now,
        )?;

        // Then canonical page transition behavior is applied.
        assert_eq!(model.active_page, target_page);
        assert!(!model.gpio_gesture.is_active());
    }
    Ok(())
}

#[test]
fn left_down_on_the_active_tab_is_inert() -> Result<()> {
    for page in [
        ActivePage::Controls,
        ActivePage::SavedConfig,
        ActivePage::Status,
    ] {
        // Given the active tab and an active GPIO gesture.
        let now = Instant::now();
        let mut model = model();
        model.set_page(page);
        draw_sized(&mut model, 80, 24)?;
        let rect = tab_rect(&model, page)?;
        seed_gesture(&mut model, now);

        // When the active label receives left-button Down.
        handle_mouse_at(
            &mut model,
            mouse(MouseEventKind::Down(MouseButton::Left), rect.x, rect.y),
            now,
        )?;

        // Then neither page nor gesture state changes.
        assert_eq!(model.active_page, page);
        assert!(model.gpio_gesture.is_active());
    }
    Ok(())
}

#[test]
fn left_down_in_each_inter_tab_gap_does_not_switch_pages() -> Result<()> {
    for (page, gap_starts) in [
        (ActivePage::Controls, [10, 24]),
        (ActivePage::SavedConfig, [8, 24]),
        (ActivePage::Status, [8, 22]),
    ] {
        for column in gap_starts.into_iter().flat_map(|start| [start, start + 1]) {
            // Given either cell of an inter-tab gap.
            let mut model = model();
            model.set_page(page);
            draw_sized(&mut model, 80, 24)?;

            // When that gap receives left-button Down.
            handle_mouse_at(
                &mut model,
                mouse(MouseEventKind::Down(MouseButton::Left), column, 2),
                Instant::now(),
            )?;

            // Then page selection is unchanged.
            assert_eq!(model.active_page, page, "column={column}");
        }
    }
    Ok(())
}

#[test]
fn middle_right_and_scroll_events_on_an_inactive_tab_are_inert() -> Result<()> {
    for kind in [
        MouseEventKind::Down(MouseButton::Middle),
        MouseEventKind::Down(MouseButton::Right),
        MouseEventKind::ScrollDown,
        MouseEventKind::ScrollUp,
        MouseEventKind::ScrollLeft,
        MouseEventKind::ScrollRight,
    ] {
        // Given an inactive rendered tab and an active GPIO gesture.
        let now = Instant::now();
        let mut model = model();
        draw_sized(&mut model, 80, 24)?;
        let rect = tab_rect(&model, ActivePage::SavedConfig)?;
        seed_gesture(&mut model, now);

        // When any event other than left-button Down reaches the label.
        handle_mouse_at(&mut model, mouse(kind, rect.x, rect.y), now)?;

        // Then page and gesture state remain unchanged.
        assert_eq!(model.active_page, ActivePage::Controls, "kind={kind:?}");
        assert!(model.gpio_gesture.is_active(), "kind={kind:?}");
    }
    Ok(())
}

#[test]
fn blocking_modal_and_error_states_keep_tab_clicks_inert() -> Result<()> {
    for blocking_state in [
        BlockingState::SavedConfigConfirmation,
        BlockingState::SavedConfigError,
        BlockingState::HardwareConfirmation,
    ] {
        // Given a rendered inactive tab behind a blocking state.
        let mut model = model();
        draw_sized(&mut model, 80, 24)?;
        let rect = tab_rect(&model, ActivePage::SavedConfig)?;
        match blocking_state {
            BlockingState::SavedConfigConfirmation => {
                model.saved_config.confirmation = Some(ConfigConfirmation::Save {
                    items: Vec::new(),
                    dangerous: Vec::new(),
                });
            }
            BlockingState::SavedConfigError => {
                model.saved_config.error = Some("failure".to_string());
            }
            BlockingState::HardwareConfirmation => {
                model.hardware_confirm = Some(power_confirmation());
            }
        }

        // When the covered tab receives left-button Down.
        handle_mouse_at(
            &mut model,
            mouse(MouseEventKind::Down(MouseButton::Left), rect.x, rect.y),
            Instant::now(),
        )?;

        // Then modal/error precedence leaves the current page selected.
        assert_eq!(
            model.active_page,
            ActivePage::Controls,
            "blocking_state={blocking_state:?}"
        );
    }
    Ok(())
}
