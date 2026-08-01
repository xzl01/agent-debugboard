use serde::{de::Error, Deserialize, Deserializer, Serialize};

#[derive(Debug, Clone, PartialEq, Eq, Hash, Deserialize, Serialize)]
#[serde(transparent)]
pub struct ConfigItemId(pub String);

impl ConfigItemId {
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ConfigItemKind {
    Power,
    Switch,
    Gpio,
    Unknown(String),
}

impl<'de> Deserialize<'de> for ConfigItemKind {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        Ok(match String::deserialize(deserializer)?.as_str() {
            "power" => Self::Power,
            "switch" => Self::Switch,
            "gpio" => Self::Gpio,
            value => Self::Unknown(value.to_string()),
        })
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PowerState {
    On,
    Off,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum GpioDirection {
    Input,
    Output,
}

#[derive(Debug, Clone)]
pub enum GpioLevel {
    Low,
    High,
}

impl<'de> Deserialize<'de> for GpioLevel {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        match u8::deserialize(deserializer)? {
            0 => Ok(Self::Low),
            1 => Ok(Self::High),
            value => Err(D::Error::custom(format!("invalid GPIO value {value}"))),
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
pub struct ConfigPowerValue {
    pub state: PowerState,
}
#[derive(Debug, Clone, Deserialize)]
pub struct ConfigSwitchValue {
    pub route: String,
}
#[derive(Debug, Clone, Deserialize)]
pub struct ConfigGpioValue {
    pub direction: GpioDirection,
    pub value: GpioLevel,
}
#[derive(Debug, Clone, Deserialize)]
pub struct UnknownConfigValue {}

#[derive(Debug, Clone, Deserialize)]
#[serde(untagged)]
pub enum ConfigValue {
    Power(ConfigPowerValue),
    Switch(ConfigSwitchValue),
    Gpio(ConfigGpioValue),
    Unknown(UnknownConfigValue),
}

#[derive(Debug, Clone, Deserialize)]
struct PersistentConfigItemWire {
    id: ConfigItemId,
    kind: ConfigItemKind,
    #[serde(default)]
    current: Present<Option<ConfigValue>>,
    #[serde(default)]
    saved: Present<Option<ConfigValue>>,
    #[serde(default)]
    selected: Present<bool>,
    #[serde(default)]
    requires_confirm: Present<Option<bool>>,
    #[serde(default)]
    apply_state: Present<Option<crate::persistent_config::ConfigApplyState>>,
}

#[derive(Debug, Clone)]
pub struct PersistentConfigItem {
    pub id: ConfigItemId,
    pub kind: ConfigItemKind,
    pub current: Option<ConfigValue>,
    pub saved: Option<ConfigValue>,
    pub selected: bool,
    pub requires_confirm: Option<bool>,
    pub apply_state: Option<crate::persistent_config::ConfigApplyState>,
    pub(crate) current_present: bool,
    pub(crate) saved_present: bool,
    pub(crate) selected_present: bool,
    pub(crate) requires_confirm_present: bool,
    pub(crate) apply_state_present: bool,
}

impl<'de> Deserialize<'de> for PersistentConfigItem {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let wire = PersistentConfigItemWire::deserialize(deserializer)?;
        Ok(Self {
            current: bind_value(&wire.kind, wire.current.value).map_err(D::Error::custom)?,
            saved: bind_value(&wire.kind, wire.saved.value).map_err(D::Error::custom)?,
            id: wire.id,
            kind: wire.kind,
            selected: wire.selected.value,
            requires_confirm: wire.requires_confirm.value,
            apply_state: wire.apply_state.value,
            current_present: wire.current.present,
            saved_present: wire.saved.present,
            selected_present: wire.selected.present,
            requires_confirm_present: wire.requires_confirm.present,
            apply_state_present: wire.apply_state.present,
        })
    }
}

#[derive(Debug, Clone)]
pub(crate) struct Present<T> {
    pub(crate) value: T,
    pub(crate) present: bool,
}

impl<T: Default> Default for Present<T> {
    fn default() -> Self {
        Self {
            value: T::default(),
            present: false,
        }
    }
}

impl<'de, T: Deserialize<'de>> Deserialize<'de> for Present<T> {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        Ok(Self {
            value: T::deserialize(deserializer)?,
            present: true,
        })
    }
}

fn bind_value(
    kind: &ConfigItemKind,
    value: Option<ConfigValue>,
) -> Result<Option<ConfigValue>, &'static str> {
    match kind {
        ConfigItemKind::Power => match value {
            value @ (None | Some(ConfigValue::Power(_))) => Ok(value),
            _ => Err("config value does not match item kind"),
        },
        ConfigItemKind::Switch => match value {
            value @ (None | Some(ConfigValue::Switch(_))) => Ok(value),
            _ => Err("config value does not match item kind"),
        },
        ConfigItemKind::Gpio => match value {
            value @ (None | Some(ConfigValue::Gpio(_))) => Ok(value),
            _ => Err("config value does not match item kind"),
        },
        ConfigItemKind::Unknown(_) => match value {
            None => Ok(None),
            Some(_) => Ok(Some(ConfigValue::Unknown(UnknownConfigValue {}))),
        },
    }
}
