use super::model::TuiModel;
use super::status_page::{status_header_line, status_lines};
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::{TuiStatusSwitchInfo, WsStatusSnapshot};
use anyhow::{anyhow, Result};
use std::time::Duration;

fn model_with_switch() -> TuiModel {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    let mut snapshot = WsStatusSnapshot::default();
    snapshot.switches.insert(
        "sd".to_string(),
        TuiStatusSwitchInfo {
            route: "target".to_string(),
            routes: vec!["target".to_string(), "usb-reader".to_string()],
            ..Default::default()
        },
    );
    model.apply_status_snapshot(snapshot);
    model
}

fn char_index(text: &str, needle: &str) -> Result<usize> {
    let byte = text
        .find(needle)
        .ok_or_else(|| anyhow!("missing {needle:?} in {text:?}"))?;
    Ok(text[..byte].chars().count())
}

#[test]
fn status_header_columns_align_with_data_cells() -> Result<()> {
    let model = model_with_switch();
    for width in [80usize, 120] {
        let header = status_header_line(width).to_string();
        let data = status_lines(&model, width)
            .first()
            .map(ToString::to_string)
            .ok_or_else(|| anyhow!("status switch row is missing"))?;

        assert_eq!(char_index(&header, "SWITCH")?, 0, "header={header:?}");
        assert_eq!(char_index(&header, "DESIRED")?, 12, "header={header:?}");
        assert_eq!(char_index(&header, "ACTUAL")?, 26, "header={header:?}");
        assert_eq!(char_index(&header, "STATE")?, 40, "header={header:?}");

        assert_eq!(char_index(&data, "sd")?, 0, "data={data:?}");
        let targets: Vec<usize> = data
            .match_indices("target")
            .map(|(byte, _)| data[..byte].chars().count())
            .collect();
        assert_eq!(targets, vec![12, 26], "data={data:?}");
        assert_eq!(char_index(&data, "ready")?, 40, "data={data:?}");
    }
    Ok(())
}

#[test]
fn status_header_pads_titles_to_column_widths() -> Result<()> {
    let header = status_header_line(80).to_string();
    // Given padding, the next title must start immediately after gap+column.
    let desired_end = char_index(&header, "DESIRED")? + "DESIRED".len();
    let actual_at = char_index(&header, "ACTUAL")?;
    assert_eq!(actual_at, 26);
    assert!(desired_end < actual_at);
    assert!(header[..actual_at].ends_with("  "), "header={header:?}");
    Ok(())
}
