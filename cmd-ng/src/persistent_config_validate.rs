use crate::persistent_config::{
    ConfigAction, ConfigBusyActivity, PersistentConfigEnvelope, PersistentConfigResponse,
    PersistentConfigSnapshot,
};
use crate::persistent_config_value::Present;
use anyhow::{bail, Result};
use serde::{Deserialize, Deserializer};

#[derive(Deserialize)]
struct SnapshotWire {
    present: bool,
    #[serde(default)]
    version: Present<Option<u32>>,
}

impl<'de> Deserialize<'de> for PersistentConfigSnapshot {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let wire = SnapshotWire::deserialize(deserializer)?;
        Ok(Self {
            present: wire.present,
            version: wire.version.value,
            version_present: wire.version.present,
        })
    }
}

#[derive(Deserialize)]
struct EnvelopeWire {
    schema: String,
    ok: bool,
    command: String,
    #[serde(default)]
    action: Option<ConfigAction>,
    #[serde(default)]
    backend: Option<crate::persistent_config::PersistentConfigBackend>,
    #[serde(default)]
    snapshot: Option<PersistentConfigSnapshot>,
    #[serde(default)]
    pending: Option<u32>,
    #[serde(default)]
    items: Option<Vec<crate::persistent_config::PersistentConfigItem>>,
    #[serde(default)]
    error: Option<crate::persistent_config::ConfigMachineError>,
    #[serde(default)]
    confirmation_items: Option<Vec<crate::persistent_config::ConfigItemId>>,
    #[serde(default)]
    saved_items: Option<Vec<crate::persistent_config::ConfigItemId>>,
    #[serde(default)]
    dangerous_items: Option<Vec<crate::persistent_config::ConfigItemId>>,
    #[serde(default)]
    activity: Option<ConfigBusyActivity>,
    #[serde(default)]
    applied_items: Option<Vec<crate::persistent_config::ConfigItemId>>,
    #[serde(default)]
    failed_item: Present<Option<crate::persistent_config::ConfigItemId>>,
    #[serde(default)]
    pending_items: Option<Vec<crate::persistent_config::ConfigItemId>>,
    #[serde(default)]
    noop: Option<bool>,
}

impl<'de> Deserialize<'de> for PersistentConfigEnvelope {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let wire = EnvelopeWire::deserialize(deserializer)?;
        Ok(Self {
            schema: wire.schema,
            ok: wire.ok,
            command: wire.command,
            action: wire.action,
            backend: wire.backend,
            snapshot: wire.snapshot,
            pending: wire.pending,
            items_present: wire.items.is_some(),
            items: wire.items.unwrap_or_default(),
            error: wire.error,
            confirmation_items_present: wire.confirmation_items.is_some(),
            confirmation_items: wire.confirmation_items.unwrap_or_default(),
            saved_items_present: wire.saved_items.is_some(),
            saved_items: wire.saved_items.unwrap_or_default(),
            dangerous_items_present: wire.dangerous_items.is_some(),
            dangerous_items: wire.dangerous_items.unwrap_or_default(),
            activity: wire.activity,
            applied_items_present: wire.applied_items.is_some(),
            applied_items: wire.applied_items.unwrap_or_default(),
            failed_item_present: wire.failed_item.present,
            failed_item: wire.failed_item.value,
            pending_items_present: wire.pending_items.is_some(),
            pending_items: wire.pending_items.unwrap_or_default(),
            noop: wire.noop,
        })
    }
}

impl PersistentConfigResponse {
    pub fn validate(&self, expected: &ConfigAction, status: Option<u16>) -> Result<()> {
        if self.envelope.action.as_ref() != Some(expected) {
            bail!("config response action does not match request");
        }
        if status.is_some_and(|value| (200..300).contains(&value) != self.envelope.ok) {
            bail!("config HTTP status does not match envelope ok");
        }
        if self.envelope.ok {
            self.validate_success(expected)
        } else {
            self.validate_failure()
        }
    }

    fn validate_success(&self, expected: &ConfigAction) -> Result<()> {
        let envelope = &self.envelope;
        match expected {
            ConfigAction::Get => {
                require(envelope.backend.is_some(), "backend")?;
                validate_snapshot(envelope.snapshot.as_ref())?;
                require(envelope.pending.is_some(), "pending")?;
                require(envelope.items_present, "items")?;
                for item in &envelope.items {
                    item.validate_catalog_row()?;
                }
            }
            ConfigAction::Save => {
                require(envelope.saved_items_present, "saved_items")?;
                require(envelope.confirmation_items_present, "confirmation_items")?;
                require(envelope.applied_items_present, "applied_items")?;
                validate_snapshot(envelope.snapshot.as_ref())?;
                require(envelope.pending.is_some(), "pending")?;
            }
            ConfigAction::Clear => {
                require(envelope.noop.is_some(), "noop")?;
                validate_snapshot(envelope.snapshot.as_ref())?;
                require(envelope.pending.is_some(), "pending")?;
            }
            ConfigAction::Unknown(_) => bail!("cannot validate unknown config action"),
        }
        Ok(())
    }

    fn validate_failure(&self) -> Result<()> {
        let envelope = &self.envelope;
        let error = envelope
            .error
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("config failure response is missing error"))?;
        require(!error.code.is_empty(), "error.code")?;
        require(!error.message.is_empty(), "error.message")?;
        match error.code.as_str() {
            "confirmation_required" => require(envelope.dangerous_items_present, "dangerous_items"),
            "busy" => match envelope.activity {
                Some(ConfigBusyActivity::Capture | ConfigBusyActivity::Ota) => Ok(()),
                Some(ConfigBusyActivity::Unknown(_)) | None => {
                    bail!("config busy response has invalid activity")
                }
            },
            "apply_failed" => {
                require(envelope.applied_items_present, "applied_items")?;
                require(envelope.failed_item_present, "failed_item")?;
                require(envelope.pending_items_present, "pending_items")
            }
            _ => Ok(()),
        }
    }
}

impl crate::persistent_config::PersistentConfigItem {
    fn validate_catalog_row(&self) -> Result<()> {
        require(self.current_present, "item.current")?;
        require(self.saved_present, "item.saved")?;
        require(self.selected_present, "item.selected")?;
        require(self.requires_confirm_present, "item.requires_confirm")?;
        require(self.apply_state_present, "item.apply_state")
    }
}

fn validate_snapshot(snapshot: Option<&PersistentConfigSnapshot>) -> Result<()> {
    let snapshot =
        snapshot.ok_or_else(|| anyhow::anyhow!("config response is missing snapshot"))?;
    require(snapshot.version_present, "snapshot.version")?;
    if snapshot.present {
        require(snapshot.version == Some(1), "snapshot.version==1")
    } else {
        require(snapshot.version.is_none(), "snapshot.version==null")
    }
}

fn require(condition: bool, field: &str) -> Result<()> {
    if !condition {
        bail!("config response is missing required field {field}");
    }
    Ok(())
}
