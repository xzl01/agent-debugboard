use super::controls::{navigate, Nav};
use super::events::handle_key;
use super::gpio_fixture::{generic_edge_model, item_index, projection_model, x1_2_model};
use anyhow::Result;
use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use std::time::Instant;

#[test]
fn x1_2_navigation_ends_at_maskrom_without_removed_fallback_pins() -> Result<()> {
    let mut model = x1_2_model()?;
    for width in [80, 48, 47, 32] {
        model.width = width;
        let items = super::controls::control_items(&model);
        let power_count = model.power_names.len();
        assert_eq!(power_count, 5);
        assert_eq!(
            &items[power_count..power_count + 3],
            [
                super::controls::ControlItem::Switch("sd".to_string()),
                super::controls::ControlItem::Switch("tf_wp".to_string()),
                super::controls::ControlItem::Reboot
            ]
        );
        model.control_idx = power_count - 1;
        assert_eq!(navigate(&model, Nav::Down), power_count);
        model.control_idx = power_count;
        assert_eq!(navigate(&model, Nav::Up), power_count - 1);
        model.control_idx = power_count + 1;
        assert_eq!(navigate(&model, Nav::Down), power_count + 2);
        model.control_idx = power_count + 2;
        assert_eq!(navigate(&model, Nav::Down), item_index(&model, "GP29")?);
        model.control_idx = item_index(&model, "GP29")?;
        assert_eq!(navigate(&model, Nav::Up), power_count + 2);
        model.control_idx = item_index(&model, "GP29")?;
        assert_eq!(
            navigate(&model, Nav::Right),
            item_index(&model, if width >= 48 { "GP5" } else { "GP29" })?
        );
        assert_eq!(
            navigate(&model, Nav::Down),
            item_index(&model, if width >= 48 { "GP16" } else { "GP5" })?
        );
        model.control_idx = item_index(&model, "GP5")?;
        assert_eq!(
            navigate(&model, Nav::Down),
            item_index(&model, if width >= 48 { "GP6" } else { "GP16" })?
        );
        model.control_idx = item_index(&model, "GP6")?;
        assert_eq!(
            navigate(&model, Nav::Up),
            item_index(&model, if width >= 48 { "GP5" } else { "GP16" })?
        );
        model.control_idx = item_index(&model, "GP7")?;
        assert_eq!(navigate(&model, Nav::Down), model.control_idx);
        assert_eq!(navigate(&model, Nav::PageDown), model.control_idx);
        model.control_idx = item_index(&model, "GP9")?;
        assert_eq!(
            navigate(&model, Nav::Right),
            item_index(&model, if width >= 48 { "GP8" } else { "GP9" })?
        );
        model.control_idx = item_index(&model, "GP8")?;
        assert_eq!(navigate(&model, Nav::Down), item_index(&model, "GP7")?);
        handle_key(
            &mut model,
            KeyEvent::new(KeyCode::Char('g'), KeyModifiers::NONE),
            Instant::now(),
        )?;
        assert_eq!(model.control_idx, item_index(&model, "GP29")?);
    }
    Ok(())
}

#[test]
fn metadata_order_does_not_clamp_selection_to_the_last_visual_cells_raw_index() -> Result<()> {
    let mut model = generic_edge_model()?;
    // The last visual cell is GP9, whose raw index precedes both IO pins.
    model.control_idx = item_index(&model, "GP10")?;
    assert_eq!(navigate(&model, Nav::Right), item_index(&model, "GP16")?);
    model.control_idx = item_index(&model, "GP16")?;
    assert_eq!(navigate(&model, Nav::Left), item_index(&model, "GP10")?);
    assert_eq!(navigate(&model, Nav::Down), item_index(&model, "GP9")?);
    Ok(())
}

#[test]
fn right_selects_the_metadata_sibling_cell() -> Result<()> {
    let mut model = projection_model()?;
    model.control_idx = item_index(&model, "GP10")?;
    // GP10 and GP16 share firmware row 5: Right must select the sibling.
    assert_eq!(
        navigate(&model, Nav::Right),
        item_index(&model, "GP16")?,
        "Right in paired mode must move to the sibling cell, not the raw next index"
    );
    Ok(())
}

#[test]
fn right_is_inert_when_the_firmware_row_has_no_sibling() -> Result<()> {
    let mut model = projection_model()?;
    model.control_idx = item_index(&model, "GP15")?;
    // GP15 is alone on firmware row 0: there is no sibling to move to.
    assert_eq!(
        navigate(&model, Nav::Right),
        item_index(&model, "GP15")?,
        "Right must be inert on a single-cell visual row"
    );
    Ok(())
}

#[test]
fn down_preserves_the_cell_side_across_visual_rows() -> Result<()> {
    let mut model = projection_model()?;
    model.control_idx = item_index(&model, "GP16")?;
    // GP16 is the right cell of its visual row; the next visual row
    // [GP8, GP9] maps right to GP9.
    assert_eq!(
        navigate(&model, Nav::Down),
        item_index(&model, "GP9")?,
        "Down must land on the same side of the next visual row"
    );
    Ok(())
}

#[test]
fn up_preserves_the_cell_side_across_visual_rows() -> Result<()> {
    let mut model = projection_model()?;
    model.control_idx = item_index(&model, "GP9")?;
    assert_eq!(
        navigate(&model, Nav::Up),
        item_index(&model, "GP16")?,
        "Up must land on the same side of the previous visual row"
    );
    Ok(())
}

#[test]
fn page_down_steps_three_visual_rows() -> Result<()> {
    let mut model = projection_model()?;
    model.control_idx = item_index(&model, "GP10")?;
    // Visual rows: [GP15], [GP10,GP16], [GP8,GP9], [GP20]. From row 1, a
    // three-row page step clamps to the last row, whose left cell is GP20.
    assert_eq!(
        navigate(&model, Nav::PageDown),
        item_index(&model, "GP20")?,
        "PageDown must step three visual rows, not three raw indices"
    );
    Ok(())
}

#[test]
fn g_jumps_to_the_first_projected_gpio() -> Result<()> {
    let mut model = projection_model()?;
    handle_key(
        &mut model,
        KeyEvent::new(KeyCode::Char('g'), KeyModifiers::NONE),
        Instant::now(),
    )?;
    assert_eq!(
        model.control_idx,
        item_index(&model, "GP15")?,
        "g must select the left cell of the first projected GPIO visual row, \
         which is firmware row 0 (GP15), not the first snapshot entry (GP10)"
    );
    Ok(())
}
