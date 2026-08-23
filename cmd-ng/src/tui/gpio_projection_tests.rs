use super::gpio_fixture::{draw, find_row, item_index, projection_model, row_text};
use anyhow::Result;
use ratatui::style::Color;

#[test]
fn gpio_cells_render_group_label_marker_and_state_words() -> Result<()> {
    let mut model = projection_model()?;
    let buffer = draw(&mut model, 80, 24)?;
    let rows: Vec<String> = (0..buffer.area.height)
        .map(|y| row_text(&buffer, y))
        .collect();
    let text = rows.join("\n");
    for expected in [
        "J16 GP15 ● OUT HIGH",
        "J16 GP10 ● OUT HIGH",
        "J16 GP16 ○ OUT LOW",
        "J13 RSET ◌ IN LOW",
        "J13 USER ◌ IN HIGH",
        "GPIO GP20 ◌ IN LOW",
    ] {
        assert!(
            text.contains(expected),
            "missing GPIO cell {expected:?} in:\n{text}"
        );
    }
    Ok(())
}

#[test]
fn gpio_projection_orders_by_firmware_row_not_snapshot_order() -> Result<()> {
    let mut model = projection_model()?;
    let buffer = draw(&mut model, 80, 24)?;
    // GP15 is last in the J16 snapshot order but sits on firmware row 0, so
    // its visual row precedes the row-5 pair GP10/GP16.
    let gp15 = find_row(&buffer, "GP15")?;
    let gp10 = find_row(&buffer, "GP10")?;
    assert!(
        gp15 < gp10,
        "firmware row 0 cell must precede row 5 cells: GP15@{gp15} GP10@{gp10}"
    );
    Ok(())
}

#[test]
fn gpio_single_firmware_row_keeps_the_other_half_empty() -> Result<()> {
    let mut model = projection_model()?;
    let buffer = draw(&mut model, 80, 24)?;
    let row = find_row(&buffer, "GP15")?;
    let text = row_text(&buffer, row);
    let right_half: String = text.chars().skip(40).collect();
    assert!(
        !right_half.contains("GP") && !right_half.contains("OUT") && !right_half.contains("IN"),
        "right half of a single-cell firmware row must stay empty: {right_half:?}"
    );
    Ok(())
}

#[test]
fn gpio_pairs_two_cells_at_eighty_columns_and_splits_below_forty_eight() -> Result<()> {
    let mut model = projection_model()?;
    let buffer = draw(&mut model, 80, 24)?;
    assert_eq!(
        find_row(&buffer, "GP10")?,
        find_row(&buffer, "GP16")?,
        "GP10 and GP16 share firmware row 5 and must pair at 80 columns"
    );

    let mut model = projection_model()?;
    model.width = 47;
    let buffer = draw(&mut model, 47, 24)?;
    let gp10 = find_row(&buffer, "GP10")?;
    let gp16 = find_row(&buffer, "GP16")?;
    assert_ne!(
        gp10, gp16,
        "below 48 columns every GPIO cell owns a full visual row"
    );
    assert!(
        !row_text(&buffer, gp10).contains("GP16"),
        "single-cell row must not carry a second pin"
    );
    Ok(())
}

#[test]
fn selected_paired_gpio_cell_paints_only_its_half() -> Result<()> {
    let mut model = projection_model()?;
    model.control_idx = item_index(&model, "GP10")?;
    let buffer = draw(&mut model, 80, 24)?;
    let row = find_row(&buffer, "GP10")?;
    for x in 0..40u16 {
        assert_eq!(
            buffer[(x, row)].bg,
            Color::White,
            "left half of the selected GP10 cell must be White at x={x}"
        );
    }
    for x in 40..80u16 {
        assert_ne!(
            buffer[(x, row)].bg,
            Color::White,
            "sibling half of a paired row must not take the selection background at x={x}"
        );
    }
    Ok(())
}
