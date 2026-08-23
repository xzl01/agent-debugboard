use crate::monitoring::BoardMonitoring;
use crate::persistent_config::PersistentConfigStatus;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct TuiStatusPowerOutput {
    pub name: String,
    pub state: String,
    pub value: i32,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct TuiStatusGpio {
    pub name: String,
    #[serde(default)]
    pub pin: u32,
    #[serde(default)]
    pub value: Option<i32>,
    #[serde(default)]
    pub direction: String,
    #[serde(default)]
    pub note: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub layout_group: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub layout_label: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub layout_row: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub layout_column: Option<u32>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct TuiStatusSwitchInfo {
    #[serde(default)]
    pub route: String,
    #[serde(default)]
    pub routes: Vec<String>,
    #[serde(default)]
    pub requires_confirm: bool,
}

pub type TuiStatusSwitches = BTreeMap<String, TuiStatusSwitchInfo>;

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct TuiStatusWatchdog {
    pub automatic: bool,
    pub healthy: bool,
    pub supported: bool,
    pub armed: bool,
    pub timeout_ms: u32,
    pub bootloader_on_timeout: bool,
    #[serde(default)]
    pub failing_service: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct WsStatusSnapshot {
    #[serde(default)]
    pub r#type: String,
    #[serde(default)]
    pub topic: String,
    #[serde(default)]
    pub schema: String,
    pub sequence: Option<u64>,
    #[serde(default)]
    pub power_outputs: Vec<TuiStatusPowerOutput>,
    #[serde(default)]
    pub switches: TuiStatusSwitches,
    #[serde(default)]
    pub watchdog: TuiStatusWatchdog,
    #[serde(default)]
    pub gpios: Vec<TuiStatusGpio>,
    #[serde(default)]
    pub board_monitoring: BoardMonitoring,
    #[serde(default)]
    pub config: Option<PersistentConfigStatus>,
}
