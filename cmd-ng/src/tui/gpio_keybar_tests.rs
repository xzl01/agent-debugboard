use super::gpio_fixture::{draw, item_index, projection_model, row_text};
use super::model::TuiModel;
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::WsStatusSnapshot;
use anyhow::Result;
use std::time::Duration;

const GPIO_SEGMENTS: [&str; 4] = ["l LOW", "o HIGH", "i INPUT", "Mouse click/hold/2x"];

#[test]
fn gpio_keybar_advertises_direct_actions_and_mouse_gestures() -> Result<()> {
    for (width, height) in [(80u16, 24u16), (120, 32)] {
        let mut model = projection_model()?;
        model.control_idx = item_index(&model, "GP10")?;
        let buffer = draw(&mut model, width, height)?;
        let keybar = row_text(&buffer, height - 1);
        for segment in GPIO_SEGMENTS {
            assert!(
                keybar.contains(segment),
                "keybar at {width} must advertise {segment:?}: {keybar:?}"
            );
        }
        let order: Vec<usize> = GPIO_SEGMENTS
            .iter()
            .map(|segment| keybar.find(segment).unwrap_or(usize::MAX))
            .collect();
        assert!(
            order.windows(2).all(|pair| pair[0] < pair[1]),
            "GPIO keybar segments must keep the contract order: {keybar:?}"
        );
        for retired in ["Enter/Space/0", "0 LOW", "1 HIGH", "L/M/R"] {
            assert!(
                !keybar.contains(retired),
                "keybar at {width} must not advertise retired segment {retired:?}: {keybar:?}"
            );
        }
    }
    Ok(())
}

#[test]
fn gpio_keybar_keeps_trailing_segment_order() -> Result<()> {
    let mut model = projection_model()?;
    model.control_idx = item_index(&model, "GP10")?;
    let buffer = draw(&mut model, 120, 32)?;
    let keybar = row_text(&buffer, 31);
    let trailing = [
        "Mouse click/hold/2x",
        "Tab/Shift+Tab page",
        "g GPIO",
        "p pause",
        "r refresh",
        "PgUp/PgDn Move",
        "q quit",
    ];
    for segment in trailing {
        assert!(
            keybar.contains(segment),
            "keybar at 120 must fit {segment:?}: {keybar:?}"
        );
    }
    let order: Vec<usize> = trailing
        .iter()
        .map(|segment| keybar.find(segment).unwrap_or(usize::MAX))
        .collect();
    assert!(
        order.windows(2).all(|pair| pair[0] < pair[1]),
        "trailing segments keep their frozen order after the GPIO block: {keybar:?}"
    );
    Ok(())
}

#[test]
fn gpio_keybar_drops_whole_segments_at_narrow_widths() -> Result<()> {
    let mut model = projection_model()?;
    model.control_idx = item_index(&model, "GP10")?;
    let buffer = draw(&mut model, 47, 24)?;
    let keybar = row_text(&buffer, 23);
    for segment in GPIO_SEGMENTS {
        assert!(
            keybar.contains(segment),
            "all four GPIO segments fit at 47: {keybar:?}"
        );
    }
    assert!(
        !keybar.contains("Tab/Shift+Tab"),
        "navigation segments yield at 47: {keybar:?}"
    );

    // The four GPIO segments need 43 columns; at 42 the Mouse segment is the
    // first one that no longer fits and must drop whole.
    let mut model = projection_model()?;
    model.control_idx = item_index(&model, "GP10")?;
    let buffer = draw(&mut model, 42, 24)?;
    let keybar = row_text(&buffer, 23);
    assert!(keybar.contains("i INPUT"), "keybar={keybar:?}");
    assert!(
        !keybar.contains("Mouse click/hold/2x"),
        "the first over-wide segment drops whole: {keybar:?}"
    );
    assert!(
        !keybar.contains("click/hold"),
        "no partial Mouse label survives: {keybar:?}"
    );
    Ok(())
}

#[test]
fn non_gpio_selection_keeps_the_general_keybar() -> Result<()> {
    let mut model = projection_model()?;
    model.control_idx = item_index(&model, "GP10")?;
    let gpio_keybar = row_text(&draw(&mut model, 120, 32)?, 31);
    assert!(
        gpio_keybar.contains("Mouse click/hold/2x"),
        "{gpio_keybar:?}"
    );

    let snapshot = WsStatusSnapshot {
        power_outputs: super::gpio_fixture::current_power_outputs(),
        ..Default::default()
    };
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_status_snapshot(snapshot);
    model.control_idx = 0;
    let buffer = draw(&mut model, 120, 32)?;
    let keybar = row_text(&buffer, 31);
    assert!(keybar.contains("Enter/click activate"), "{keybar:?}");
    assert!(
        !keybar.contains("i input"),
        "the general keybar must not advertise a GPIO-only binding: {keybar:?}"
    );
    assert!(
        !keybar.contains("Mouse click/hold/2x"),
        "non-GPIO selection must not advertise mouse gestures: {keybar:?}"
    );
    Ok(())
}
