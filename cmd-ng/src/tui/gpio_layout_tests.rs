use super::gpio_fixture::{projection_model, STATUS_JSON};
use crate::ws_status::{TuiStatusGpio, WsStatusSnapshot};
use anyhow::{anyhow, Result};

fn find_gpio<'a>(snapshot: &'a WsStatusSnapshot, name: &str) -> Result<&'a TuiStatusGpio> {
    snapshot
        .gpios
        .iter()
        .find(|gpio| gpio.name == name)
        .ok_or_else(|| anyhow!("{name} missing from fixture snapshot"))
}

#[test]
fn status_json_preserves_camel_case_layout_fields() -> Result<()> {
    let snapshot: WsStatusSnapshot =
        serde_json::from_str(STATUS_JSON).map_err(anyhow::Error::msg)?;

    let gp10 = find_gpio(&snapshot, "GP10")?;
    assert_eq!(gp10.layout_group.as_deref(), Some("J16"));
    assert_eq!(gp10.layout_label.as_deref(), Some("GP10"));
    assert_eq!(gp10.layout_row, Some(5));
    assert_eq!(gp10.layout_column, Some(0));

    // Numeric zero is distinguishable from absent.
    let gp15 = find_gpio(&snapshot, "GP15")?;
    assert_eq!(gp15.layout_row, Some(0));
    assert_eq!(gp15.layout_column, Some(0));

    let gp20 = find_gpio(&snapshot, "GP20")?;
    assert_eq!(gp20.layout_group, None);
    assert_eq!(gp20.layout_label, None);
    assert_eq!(gp20.layout_row, None);
    assert_eq!(gp20.layout_column, None);
    Ok(())
}

#[test]
fn model_associates_layouts_by_gpio_name() -> Result<()> {
    let model = projection_model()?;
    let gp10 = model
        .gpio_layouts
        .get("GP10")
        .ok_or_else(|| anyhow!("GP10 layout missing from model"))?;
    assert_eq!(gp10.pin, 10);
    assert_eq!(gp10.group.as_deref(), Some("J16"));
    assert_eq!(gp10.label.as_deref(), Some("GP10"));
    assert_eq!(gp10.row, Some(5));
    assert_eq!(gp10.column, Some(0));

    let gp8 = model
        .gpio_layouts
        .get("GP8")
        .ok_or_else(|| anyhow!("GP8 layout missing from model"))?;
    assert_eq!(gp8.group.as_deref(), Some("J13"));
    assert_eq!(gp8.label.as_deref(), Some("RSET"));

    let gp20 = model
        .gpio_layouts
        .get("GP20")
        .ok_or_else(|| anyhow!("GP20 layout entry missing from model"))?;
    assert_eq!(gp20.pin, 20);
    assert_eq!(gp20.group, None);
    assert_eq!(gp20.row, None);
    Ok(())
}

#[test]
fn con_mas_gpios_keep_their_layout_entries() -> Result<()> {
    let snapshot: WsStatusSnapshot = serde_json::from_str(
        r#"{"gpios":[
          {"name":"GP7","pin":7,"value":0,"direction":"input","note":"CON_MAS","layoutGroup":"J13","layoutLabel":"MASKROM","layoutRow":1,"layoutColumn":1},
          {"name":"GP8","pin":8,"value":0,"direction":"input","note":"CON_REST","layoutGroup":"J13","layoutLabel":"RSET","layoutRow":0,"layoutColumn":0}
        ]}"#,
    )
    .map_err(anyhow::Error::msg)?;
    let mut model = super::model::TuiModel::new(
        crate::client::DEFAULT_BASE_URL.to_string(),
        std::time::Duration::from_secs(2),
    );
    model.apply_status_snapshot(snapshot);

    assert_eq!(model.gpio_names, vec!["GP7".to_string(), "GP8".to_string()]);
    assert_eq!(model.gpio_levels.get("GP7"), Some(&false));
    assert_eq!(model.gpio_is_input.get("GP7"), Some(&true));
    assert_eq!(model.gpio_notes.get("GP7"), Some(&"CON_MAS".to_string()));
    let gp7 = model
        .gpio_layouts
        .get("GP7")
        .ok_or_else(|| anyhow!("GP7 layout entry missing from model"))?;
    assert_eq!(gp7.pin, 7);
    assert_eq!(gp7.group.as_deref(), Some("J13"));
    assert_eq!(gp7.label.as_deref(), Some("MASKROM"));
    assert_eq!(gp7.row, Some(1));
    assert_eq!(gp7.column, Some(1));
    assert!(model.gpio_layouts.contains_key("GP8"));
    Ok(())
}

#[test]
fn later_snapshot_replaces_stale_layout_metadata() -> Result<()> {
    let mut model = projection_model()?;
    let second: WsStatusSnapshot = serde_json::from_str(
        r#"{"gpios":[
          {"name":"GP10","pin":10,"value":1,"direction":"output","note":"J16_PIN1"},
          {"name":"GP9","pin":9,"value":1,"direction":"input","note":"CON_USER","layoutGroup":"J13","layoutLabel":"USER","layoutRow":0,"layoutColumn":1}
        ]}"#,
    )
    .map_err(anyhow::Error::msg)?;
    model.apply_status_snapshot(second);

    let gp10 = model
        .gpio_layouts
        .get("GP10")
        .ok_or_else(|| anyhow!("GP10 layout entry missing after refresh"))?;
    assert_eq!(gp10.pin, 10);
    assert_eq!(gp10.group, None, "stale group must be cleared");
    assert_eq!(gp10.row, None, "stale row must be cleared");
    assert!(!model.gpio_layouts.contains_key("GP8"));
    assert!(!model.gpio_layouts.contains_key("GP15"));

    let gp9 = model
        .gpio_layouts
        .get("GP9")
        .ok_or_else(|| anyhow!("GP9 layout missing after refresh"))?;
    assert_eq!(gp9.group.as_deref(), Some("J13"));
    assert_eq!(gp9.column, Some(1));
    Ok(())
}

#[test]
fn empty_layout_strings_are_absent_at_the_model_boundary() -> Result<()> {
    let snapshot: WsStatusSnapshot = serde_json::from_str(
        r#"{"gpios":[
          {"name":"GP10","pin":10,"value":1,"direction":"output","note":"J16_PIN1","layoutGroup":"","layoutLabel":"","layoutRow":5,"layoutColumn":0}
        ]}"#,
    )
    .map_err(anyhow::Error::msg)?;
    let mut model = super::model::TuiModel::new(
        crate::client::DEFAULT_BASE_URL.to_string(),
        std::time::Duration::from_secs(2),
    );
    model.apply_status_snapshot(snapshot);

    let gp10 = model
        .gpio_layouts
        .get("GP10")
        .ok_or_else(|| anyhow!("GP10 layout entry missing"))?;
    assert_eq!(gp10.group, None);
    assert_eq!(gp10.label, None);
    assert_eq!(gp10.row, Some(5));
    assert_eq!(gp10.column, Some(0));
    Ok(())
}
