use crate::persistent_config::{
    ConfigAction, ConfigApplyState, ConfigBusyActivity, ConfigItemId, ConfigItemKind, ConfigValue,
    GpioDirection, GpioLevel, PersistentConfigEnvelope, PowerState,
};
use anyhow::Result;
use std::io::Write;

pub(crate) fn write_show(
    envelope: &PersistentConfigEnvelope,
    stdout: &mut dyn Write,
) -> Result<()> {
    if let Some(action) = &envelope.action {
        writeln!(stdout, "action={}", action_text(action))?;
    }
    if let Some(backend) = &envelope.backend {
        writeln!(
            stdout,
            "backend_available={} backend_reason={}",
            backend.available, backend.reason
        )?;
    }
    if let Some(snapshot) = &envelope.snapshot {
        writeln!(
            stdout,
            "snapshot_present={} snapshot_version={}",
            snapshot.present,
            optional_version(snapshot.version)
        )?;
    }
    writeln!(stdout, "pending={}", optional_count(envelope.pending))?;
    for item in &envelope.items {
        writeln!(
            stdout,
            "{} kind={} current={} saved={} selected={} requires_confirm={} apply_state={}",
            item.id.as_str(),
            kind_text(&item.kind),
            value_text(item.current.as_ref()),
            value_text(item.saved.as_ref()),
            item.selected,
            optional_bool(item.requires_confirm),
            optional_state(item.apply_state.as_ref())
        )?;
    }
    Ok(())
}

pub(crate) fn write_save(
    envelope: &PersistentConfigEnvelope,
    stdout: &mut dyn Write,
) -> Result<()> {
    writeln!(
        stdout,
        "config save: saved_items={} confirmation_items={} pending={}",
        ids_text(&envelope.saved_items),
        ids_text(&envelope.confirmation_items),
        optional_count(envelope.pending)
    )?;
    Ok(())
}

pub(crate) fn write_apply(
    envelope: &PersistentConfigEnvelope,
    stdout: &mut dyn Write,
) -> Result<()> {
    writeln!(
        stdout,
        "config apply: noop={} applied_items={} failed_item={} pending_items={}",
        envelope.noop.unwrap_or(false),
        ids_text(&envelope.applied_items),
        optional_id(&envelope.failed_item),
        ids_text(&envelope.pending_items)
    )?;
    Ok(())
}

pub(crate) fn write_clear(
    envelope: &PersistentConfigEnvelope,
    stdout: &mut dyn Write,
) -> Result<()> {
    let present = envelope
        .snapshot
        .as_ref()
        .map(|snapshot| snapshot.present)
        .unwrap_or(false);
    writeln!(
        stdout,
        "config clear: snapshot_present={present} pending={}; current hardware unchanged",
        optional_count(envelope.pending)
    )?;
    Ok(())
}

pub(crate) fn error_text(envelope: &PersistentConfigEnvelope) -> String {
    let error = envelope.error.as_ref();
    let mut text = format!(
        "{}: {}",
        error.map_or("config_error", |value| value.code.as_str()),
        error.map_or("board returned config error", |value| value
            .message
            .as_str())
    );
    if !envelope.dangerous_items.is_empty() {
        text.push_str(&format!(
            " dangerous_items={}",
            ids_text(&envelope.dangerous_items)
        ));
    }
    if let Some(activity) = &envelope.activity {
        text.push_str(&format!(" activity={}", activity_text(activity)));
    }
    if !envelope.applied_items.is_empty()
        || envelope.failed_item.is_some()
        || !envelope.pending_items.is_empty()
    {
        text.push_str(&format!(
            " applied_items={} failed_item={} pending_items={}",
            ids_text(&envelope.applied_items),
            optional_id(&envelope.failed_item),
            ids_text(&envelope.pending_items)
        ));
    }
    text
}

fn ids_text(ids: &[ConfigItemId]) -> String {
    ids.iter()
        .map(ConfigItemId::as_str)
        .collect::<Vec<_>>()
        .join(",")
}
fn optional_id(item: &Option<ConfigItemId>) -> &str {
    item.as_ref().map_or("null", ConfigItemId::as_str)
}
fn optional_count(count: Option<u32>) -> String {
    count.map_or_else(|| "unknown".to_string(), |value| value.to_string())
}
fn optional_version(version: Option<u32>) -> String {
    version.map_or_else(|| "null".to_string(), |value| value.to_string())
}
fn optional_bool(value: Option<bool>) -> String {
    value.map_or_else(|| "null".to_string(), |value| value.to_string())
}
fn optional_state(state: Option<&ConfigApplyState>) -> String {
    state.map_or_else(|| "unknown".to_string(), apply_state_text)
}
fn kind_text(kind: &ConfigItemKind) -> &str {
    match kind {
        ConfigItemKind::Power => "power",
        ConfigItemKind::Switch => "switch",
        ConfigItemKind::Gpio => "gpio",
        ConfigItemKind::Unknown(value) => value,
    }
}
fn apply_state_text(state: &ConfigApplyState) -> String {
    match state {
        ConfigApplyState::NotSaved => "not_saved".to_string(),
        ConfigApplyState::Applied => "applied".to_string(),
        ConfigApplyState::Pending => "pending".to_string(),
        ConfigApplyState::Failed => "failed".to_string(),
        ConfigApplyState::Unknown(value) => value.clone(),
    }
}
fn action_text(action: &ConfigAction) -> &str {
    match action {
        ConfigAction::Get => "get",
        ConfigAction::Save => "save",
        ConfigAction::Apply => "apply",
        ConfigAction::Clear => "clear",
        ConfigAction::Unknown(value) => value,
    }
}
fn value_text(value: Option<&ConfigValue>) -> String {
    match value {
        Some(ConfigValue::Power(value)) => format!("state={}", power_state_text(&value.state)),
        Some(ConfigValue::Switch(value)) => format!("route={}", value.route),
        Some(ConfigValue::Gpio(value)) => {
            format!(
                "direction={} value={}",
                gpio_direction_text(&value.direction),
                gpio_level_text(&value.value)
            )
        }
        Some(ConfigValue::Unknown(_)) => "unknown".to_string(),
        None => "null".to_string(),
    }
}
fn power_state_text(state: &PowerState) -> &str {
    match state {
        PowerState::On => "on",
        PowerState::Off => "off",
    }
}
fn gpio_direction_text(direction: &GpioDirection) -> &str {
    match direction {
        GpioDirection::Input => "input",
        GpioDirection::Output => "output",
    }
}
fn gpio_level_text(level: &GpioLevel) -> &str {
    match level {
        GpioLevel::Low => "0",
        GpioLevel::High => "1",
    }
}
fn activity_text(activity: &ConfigBusyActivity) -> &str {
    match activity {
        ConfigBusyActivity::Capture => "capture",
        ConfigBusyActivity::Ota => "ota",
        ConfigBusyActivity::Unknown(value) => value,
    }
}
