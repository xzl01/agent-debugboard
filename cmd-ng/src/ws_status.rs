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
pub struct TuiStatusAdcChannel {
    pub name: String,
    #[serde(default)]
    pub signal: String,
    #[serde(default)]
    pub note: String,
    #[serde(default)]
    pub label: String,
    pub sensor_channel: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct TuiPdFixedProfile {
    pub voltage_mv: u16,
    pub max_current_ma: u16,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct TuiPdProgrammableRange {
    pub min_voltage_mv: u16,
    pub max_voltage_mv: u16,
    pub step_mv: u16,
    #[serde(default)]
    pub max_current_ma: u16,
    #[serde(default)]
    pub max_power_w: u16,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct TuiPdStatus {
    #[serde(default)]
    pub available: bool,
    #[serde(default)]
    pub mode: u8,
    #[serde(default)]
    pub voltage_mv: u16,
    #[serde(default)]
    pub pps_mv: u16,
    #[serde(default)]
    pub pd: bool,
    #[serde(default)]
    pub max_current_ma: u16,
    #[serde(default)]
    pub last_error: i32,
    #[serde(default)]
    pub protocol: String,
    #[serde(default)]
    pub board_max_voltage_mv: u16,
    #[serde(default)]
    pub capabilities_valid: bool,
    #[serde(default)]
    pub fixed_profiles: Vec<TuiPdFixedProfile>,
    #[serde(default)]
    pub pps_ranges: Vec<TuiPdProgrammableRange>,
    #[serde(default)]
    pub avs_ranges: Vec<TuiPdProgrammableRange>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct TuiStatusGpioLayoutGroup {
    pub name: String,
    pub edge: String,
    pub order: u32,
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
    pub adc_channels: Vec<TuiStatusAdcChannel>,
    #[serde(default)]
    pub pd: Option<TuiPdStatus>,
    #[serde(default)]
    pub switches: TuiStatusSwitches,
    #[serde(default)]
    pub watchdog: TuiStatusWatchdog,
    #[serde(default)]
    pub gpios: Vec<TuiStatusGpio>,
    #[serde(default)]
    pub gpio_layout_groups: Vec<TuiStatusGpioLayoutGroup>,
    #[serde(default)]
    pub board_monitoring: BoardMonitoring,
    #[serde(default)]
    pub config: Option<PersistentConfigStatus>,
}
