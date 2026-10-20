use super::controls::{control_items, ControlItem};
use super::gpio_fixture::{
    control_rect, draw, find_row, generic_edge_model, item_index, projection_model, row_text,
    x1_2_model, X1_2_STATUS_JSON,
};
use super::gpio_layout::TuiGpioLayoutGroup;
use super::gpio_projection::gpio_visual_rows;
use anyhow::Result;
use ratatui::style::Color;

#[test]
fn group_order_comes_from_metadata_and_undescribed_groups_stay_stable() -> Result<()> {
    let mut model = projection_model()?;
    // Existing snapshot first-appearance order is J16, J13, then fallback.
    model.gpio_layout_groups.insert(
        "J13".to_string(),
        TuiGpioLayoutGroup {
            edge: "right".to_string(),
            order: 7,
        },
    );
    let mut extra = model.gpio_layouts["GP10"].clone();
    extra.group = Some("ALPHA".to_string());
    model.gpio_layouts.insert("GP20".to_string(), extra);
    let groups: Vec<String> = gpio_visual_rows(&model)
        .iter()
        .map(|row| row.cells[0].group.clone())
        .collect();
    assert_eq!(groups, ["J13", "J16", "J16", "ALPHA"]);
    assert!(gpio_visual_rows(&model)[0].cells[0].right_edge);
    assert!(!gpio_visual_rows(&model)[1].cells[0].right_edge);

    // Equal advertised orders use first appearance, not names or hash order.
    model.gpio_layout_groups.insert(
        "J16".to_string(),
        TuiGpioLayoutGroup {
            edge: "unrecognized".to_string(),
            order: 7,
        },
    );
    let rows = gpio_visual_rows(&model);
    assert_eq!(rows[0].cells[0].group, "J16");
    assert!(!rows[0].cells[0].right_edge);
    Ok(())
}

#[test]
fn right_edge_groups_follow_order_at_every_layout_width() -> Result<()> {
    let mut model = generic_edge_model()?;
    for width in [80, 48, 47, 32] {
        model.width = width;
        let rows = gpio_visual_rows(&model);
        let names: Vec<&str> = rows
            .iter()
            .flat_map(|row| row.cells.iter().map(|cell| cell.name.as_str()))
            .collect();
        assert_eq!(names, ["GP10", "GP16", "GP8", "GP9"]);
        assert!(rows
            .iter()
            .all(|row| row.cells.len() == if width >= 48 { 2 } else { 1 }));
        assert!(rows
            .iter()
            .flat_map(|row| &row.cells)
            .all(|cell| cell.right_edge));
        for cell in rows.iter().flat_map(|row| &row.cells) {
            assert_eq!(cell.item_index, item_index(&model, &cell.name)?);
        }
    }
    // Without group metadata, restore the generic first-appearance projection.
    model.gpio_layout_groups.clear();
    let rows = gpio_visual_rows(&model);
    assert_eq!(rows[0].cells[0].group, "KEY");
    assert!(rows
        .iter()
        .flat_map(|row| &row.cells)
        .all(|cell| !cell.right_edge));
    Ok(())
}

#[test]
fn x1_2_projection_keeps_every_pin_in_physical_order_and_preserves_identity() -> Result<()> {
    let mut model = x1_2_model()?;
    let expected = [
        ("GP29", "J8", "ADC3"),
        ("GP16", "J8", "GP16"),
        ("GP15", "J8", "GP15"),
        ("GP14", "J8", "GP14"),
        ("GP13", "J8", "GP13"),
        ("GP12", "J8", "GP12"),
        ("GP9", "J13", "USER"),
        ("GP8", "J13", "RSET"),
        ("GP7", "J13", "MASKROM"),
        ("GP17", "GPIO", "GP17"),
        ("GP18", "GPIO", "GP18"),
    ];
    assert_eq!(model.gpio_names.len(), expected.len());
    for (pin, row) in [
        ("GP12", 9),
        ("GP13", 8),
        ("GP14", 7),
        ("GP15", 6),
        ("GP16", 5),
        ("GP29", 4),
    ] {
        let layout = &model.gpio_layouts[pin];
        assert_eq!(layout.group.as_deref(), Some("J8"));
        assert_eq!((layout.row, layout.column), (Some(row), Some(0)));
    }
    for (pin, row, column) in [("GP9", 0, 0), ("GP8", 0, 1), ("GP7", 1, 0)] {
        let layout = &model.gpio_layouts[pin];
        assert_eq!(layout.group.as_deref(), Some("J13"));
        assert_eq!((layout.row, layout.column), (Some(row), Some(column)));
    }
    for pin in ["GP17", "GP18"] {
        assert!(model.gpio_layouts[pin].group.is_none());
    }
    model.control_idx = item_index(&model, "GP8")?;
    for width in [80u16, 48, 47, 32] {
        model.width = width as usize;
        let rows = gpio_visual_rows(&model);
        let cells: Vec<_> = rows.iter().flat_map(|row| &row.cells).collect();
        let actual: Vec<_> = cells
            .iter()
            .map(|cell| (cell.name.as_str(), cell.group.as_str(), cell.label.as_str()))
            .collect();
        assert_eq!(
            actual, expected,
            "all 11 safe GPIOs must survive at {width}"
        );
        assert_eq!(rows.len(), if width >= 48 { 9 } else { 11 });
        for cell in &cells {
            assert_eq!(cell.item_index, item_index(&model, &cell.name)?);
            assert_eq!(cell.right_edge, cell.group != "GPIO");
        }

        let buffer = draw(&mut model, width, 32)?;
        for (name, _, _) in expected {
            let rect = control_rect(&model, name)?;
            let target = ControlItem::Gpio(name.to_string());
            for x in rect.x..rect.x + rect.width {
                assert_eq!(model.hit_map.controls.at(x, rect.y), Some(&target));
            }
        }
        let user = control_rect(&model, "GP9")?;
        let reset = control_rect(&model, "GP8")?;
        if width >= 48 {
            assert_eq!(user.y, reset.y);
            assert_eq!((user.x, reset.x), (0, width / 2));
        } else {
            assert!(user.y < reset.y);
            assert_eq!((user.x, reset.x), (0, 0));
        }
        assert_eq!(buffer[(reset.x, reset.y)].bg, Color::White);
        assert_eq!(
            control_items(&model)[model.control_idx],
            ControlItem::Gpio("GP8".to_string())
        );
        assert!(model.gpio_pending.is_none());
    }
    let mut snapshot: crate::ws_status::WsStatusSnapshot = serde_json::from_str(X1_2_STATUS_JSON)?;
    snapshot.gpios.reverse();
    model.apply_status_snapshot(snapshot);
    assert_eq!(
        control_items(&model)[model.control_idx],
        ControlItem::Gpio("GP8".to_string())
    );
    Ok(())
}

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
