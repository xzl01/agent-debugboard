use super::actions::{resolve_activation, Activation, ControlCommand, ControlIntent};
use super::controls::{control_items, ControlItem};
use super::gpio_fixture::{
    control_rect, draw, generic_edge_model, item_index, projection_model, GENERIC_EDGE_STATUS_JSON,
};
use super::gpio_gesture::GpioGestureInput;
use super::gpio_io::{GpioAction, GpioJob};
use super::runtime::{process_event, EventDisposition};
use crate::ws_status::WsStatusSnapshot;
use anyhow::{anyhow, Result};
use crossterm::event::Event;
use std::time::Instant;

#[test]
fn edge_decoration_and_pending_do_not_change_half_or_full_row_hits() -> Result<()> {
    for width in [80u16, 48, 47, 32] {
        let mut model = generic_edge_model()?;
        model.width = width as usize;
        model.control_idx = item_index(&model, "GP10")?;
        draw(&mut model, width, 24)?;
        let before: Vec<_> = model
            .hit_map
            .controls
            .iter()
            .map(|(rect, target)| (*rect, target.clone()))
            .collect();
        let left = control_rect(&model, "GP10")?;
        let right = control_rect(&model, "GP16")?;
        if width >= 48 {
            assert_eq!((left.x, left.width), (0, width / 2));
            assert_eq!((right.x, right.width), (width / 2, width - width / 2));
            assert_eq!(left.y, right.y);
        } else {
            assert_eq!((left.x, left.width), (0, width));
            assert_eq!((right.x, right.width), (0, width));
            assert_ne!(left.y, right.y);
        }
        for (rect, target) in &before {
            for x in rect.x..rect.x + rect.width {
                assert_eq!(model.hit_map.controls.at(x, rect.y), Some(target));
            }
        }
        // Remove only decoration, keeping the advertised ordering unchanged.
        for group in model.gpio_layout_groups.values_mut() {
            group.edge.clear();
        }
        model.gpio_pending = Some(GpioJob {
            action: GpioAction::DriveHigh,
            target: "GP10".to_string(),
        });
        draw(&mut model, width, 24)?;
        let after: Vec<_> = model
            .hit_map
            .controls
            .iter()
            .map(|(rect, target)| (*rect, target.clone()))
            .collect();
        assert_eq!(
            before, after,
            "decoration cannot change hitboxes at {width}"
        );
        assert_eq!(
            control_items(&model)[model.control_idx],
            ControlItem::Gpio("GP10".to_string())
        );
    }
    Ok(())
}

#[test]
fn group_reordering_and_resize_preserve_pin_identity_and_rebuild_hits() -> Result<()> {
    let mut model = generic_edge_model()?;
    model.control_idx = item_index(&model, "GP16")?;
    for width in [80u16, 48, 47, 32, 80] {
        let now = Instant::now();
        let _ = model.gpio_gesture.down(
            GpioGestureInput {
                pin: Some("GP16"),
                column: 0,
                row: 0,
            },
            now,
        );
        assert_eq!(
            process_event(&mut model, Event::Resize(width, 24), now)?,
            EventDisposition::Redraw
        );
        assert!(model.hit_map.controls.is_empty());
        assert!(!model.gpio_gesture.is_active());
        model.width = width as usize;
        draw(&mut model, width, 24)?;
        let rect = control_rect(&model, "GP16")?;
        let selected = ControlItem::Gpio("GP16".to_string());
        assert_eq!(control_items(&model)[model.control_idx], selected);
        assert_eq!(model.hit_map.controls.at(rect.x, rect.y), Some(&selected));
    }
    let mut snapshot: WsStatusSnapshot = serde_json::from_str(GENERIC_EDGE_STATUS_JSON)?;
    snapshot.gpios.reverse();
    for group in &mut snapshot.gpio_layout_groups {
        group.order = 1 - group.order;
    }
    model.apply_status_snapshot(snapshot);
    draw(&mut model, 80, 24)?;
    assert_eq!(
        control_items(&model)[model.control_idx],
        ControlItem::Gpio("GP16".to_string())
    );
    assert!(control_rect(&model, "GP8")?.y < control_rect(&model, "GP16")?.y);
    Ok(())
}

#[test]
fn paired_row_left_and_right_halves_target_different_pins() -> Result<()> {
    let mut model = projection_model()?;
    draw(&mut model, 80, 24)?;
    let row_y = control_rect(&model, "GP10")?.y;

    let left = model
        .hit_map
        .controls
        .at(2, row_y)
        .cloned()
        .ok_or_else(|| anyhow!("left half of the paired row has no hit target"))?;
    assert_eq!(left, ControlItem::Gpio("GP10".to_string()));

    let right = model
        .hit_map
        .controls
        .at(60, row_y)
        .cloned()
        .ok_or_else(|| anyhow!("right half of the paired row has no hit target"))?;
    assert_eq!(
        right,
        ControlItem::Gpio("GP16".to_string()),
        "the right half of the GP10/GP16 visual row must target GP16"
    );
    Ok(())
}

#[test]
fn empty_half_of_single_firmware_row_is_inert() -> Result<()> {
    let mut model = projection_model()?;
    draw(&mut model, 80, 24)?;
    let row_y = control_rect(&model, "GP15")?.y;
    assert!(
        model.hit_map.controls.at(60, row_y).is_none(),
        "the empty right half of the single-cell GP15 row must not hit any pin"
    );
    Ok(())
}

#[test]
fn right_half_hit_resolves_the_right_gpio_for_input_intent() -> Result<()> {
    let mut model = projection_model()?;
    draw(&mut model, 80, 24)?;
    let row_y = control_rect(&model, "GP10")?.y;
    let item = model
        .hit_map
        .controls
        .at(60, row_y)
        .cloned()
        .ok_or_else(|| anyhow!("right half of the paired row has no hit target"))?;
    assert_eq!(
        resolve_activation(&model, &item, ControlIntent::SetInput),
        Activation::Immediate(ControlCommand::SetGpioInput {
            name: "GP16".to_string(),
        }),
        "the right-half hit target must resolve set-input for GP16"
    );
    Ok(())
}

#[test]
fn right_half_hit_resolves_the_right_gpio_for_high_intent() -> Result<()> {
    let mut model = projection_model()?;
    draw(&mut model, 80, 24)?;
    let row_y = control_rect(&model, "GP10")?.y;
    let item = model
        .hit_map
        .controls
        .at(60, row_y)
        .cloned()
        .ok_or_else(|| anyhow!("right half of the paired row has no hit target"))?;
    assert_eq!(
        resolve_activation(&model, &item, ControlIntent::DriveHigh),
        Activation::Immediate(ControlCommand::SetGpioOutput {
            name: "GP16".to_string(),
            value: true,
        }),
        "the right-half hit target must resolve drive-high for GP16"
    );
    Ok(())
}
