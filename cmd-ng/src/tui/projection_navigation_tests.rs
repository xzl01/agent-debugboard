use super::controls::{navigate, Nav};
use super::events::handle_key;
use super::gpio_fixture::{item_index, projection_model};
use anyhow::Result;
use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};

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
    )?;
    assert_eq!(
        model.control_idx,
        item_index(&model, "GP15")?,
        "g must select the left cell of the first projected GPIO visual row, \
         which is firmware row 0 (GP15), not the first snapshot entry (GP10)"
    );
    Ok(())
}
