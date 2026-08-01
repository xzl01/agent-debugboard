use crate::json_contract::JSON_SCHEMA;
use anyhow::{bail, Context, Result};
use serde::{Deserialize, Deserializer, Serialize};

pub use crate::persistent_config_value::*;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ConfigAction {
    Get,
    Save,
    Apply,
    Clear,
    Unknown(String),
}

impl<'de> Deserialize<'de> for ConfigAction {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let value = String::deserialize(deserializer)?;
        Ok(match value.as_str() {
            "get" => Self::Get,
            "save" => Self::Save,
            "apply" => Self::Apply,
            "clear" => Self::Clear,
            _ => Self::Unknown(value),
        })
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ConfigApplyState {
    NotSaved,
    Applied,
    Pending,
    Failed,
    Unknown(String),
}

impl<'de> Deserialize<'de> for ConfigApplyState {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let value = String::deserialize(deserializer)?;
        Ok(match value.as_str() {
            "not_saved" => Self::NotSaved,
            "applied" => Self::Applied,
            "pending" => Self::Pending,
            "failed" => Self::Failed,
            _ => Self::Unknown(value),
        })
    }
}

#[derive(Debug, Clone, Deserialize)]
pub struct PersistentConfigBackend {
    pub available: bool,
    pub reason: String,
}

#[derive(Debug, Clone)]
pub struct PersistentConfigSnapshot {
    pub present: bool,
    pub version: Option<u32>,
    pub(crate) version_present: bool,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct PersistentConfigStatus {
    pub available: bool,
    pub reason: String,
    #[serde(default)]
    pub saved_count: u32,
    #[serde(default)]
    pub pending_count: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ConfigBusyActivity {
    Capture,
    Ota,
    Unknown(String),
}

impl<'de> Deserialize<'de> for ConfigBusyActivity {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let value = String::deserialize(deserializer)?;
        Ok(match value.as_str() {
            "capture" => Self::Capture,
            "ota" => Self::Ota,
            _ => Self::Unknown(value),
        })
    }
}

#[derive(Debug, Clone, Deserialize)]
pub struct ConfigMachineError {
    pub code: String,
    pub message: String,
}

#[derive(Debug, Clone)]
pub struct PersistentConfigEnvelope {
    pub schema: String,
    pub ok: bool,
    pub command: String,
    pub action: Option<ConfigAction>,
    pub backend: Option<PersistentConfigBackend>,
    pub snapshot: Option<PersistentConfigSnapshot>,
    pub pending: Option<u32>,
    pub items: Vec<PersistentConfigItem>,
    pub(crate) items_present: bool,
    pub error: Option<ConfigMachineError>,
    pub confirmation_items: Vec<ConfigItemId>,
    pub(crate) confirmation_items_present: bool,
    pub saved_items: Vec<ConfigItemId>,
    pub(crate) saved_items_present: bool,
    pub dangerous_items: Vec<ConfigItemId>,
    pub(crate) dangerous_items_present: bool,
    pub activity: Option<ConfigBusyActivity>,
    pub applied_items: Vec<ConfigItemId>,
    pub(crate) applied_items_present: bool,
    pub failed_item: Option<ConfigItemId>,
    pub(crate) failed_item_present: bool,
    pub pending_items: Vec<ConfigItemId>,
    pub(crate) pending_items_present: bool,
    pub noop: Option<bool>,
}

#[derive(Debug, Clone)]
pub struct PersistentConfigResponse {
    pub raw_json: String,
    pub envelope: PersistentConfigEnvelope,
}

impl PersistentConfigResponse {
    pub fn from_raw(raw_json: String) -> Result<Self> {
        let envelope: PersistentConfigEnvelope =
            serde_json::from_str(&raw_json).context("decode config response")?;
        if envelope.schema != JSON_SCHEMA || envelope.command != "config" {
            bail!("response is not a config envelope");
        }
        Ok(Self { raw_json, envelope })
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct ConfigSaveRequest<'a> {
    pub items: &'a [ConfigItemId],
    pub confirm: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct ConfigApplyRequest {
    pub confirm: bool,
}
