use super::actions::{resolve_activation, Activation, ControlCommand, ControlIntent};
use super::controls::ControlItem;
use super::gpio_fixture::{control_rect, draw, projection_model};
use anyhow::{anyhow, Result};

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
