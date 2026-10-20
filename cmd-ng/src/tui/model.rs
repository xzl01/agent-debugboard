use super::board_io::PdWorker;
use super::config_io::ConfigWorker;
use super::config_state::SavedConfigState;
use super::confirm::HardwareConfirmation;
use super::controls::control_items;
use super::gpio_io::{GpioJob, GpioWorker};
use super::gpio_layout::{TuiGpioLayout, TuiGpioLayoutGroup};
use super::hit::HitMap;
use super::pages::ActivePage;
use super::{TuiActionMsg, TUI_HISTORY_LIMIT};
use crate::adc::{AdcKind, AdcReading};
use crate::monitoring::BoardMonitoring;
use crate::ws_status::{TuiPdStatus, TuiStatusAdcChannel, WsStatusSnapshot};
use std::collections::BTreeMap;
use std::time::{Duration, Instant};

pub(super) const REBOOT_RECONNECT_TIMEOUT: Duration = Duration::from_secs(30);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct RebootReconnect {
    pub(super) accepted_at: Instant,
    pub(super) deadline: Instant,
}

#[derive(Debug, Clone, Default)]
pub(super) struct TuiSwitchState {
    pub(super) name: String,
    pub(super) desired_route: String,
    pub(super) actual_route: String,
    pub(super) routes: Vec<String>,
    pub(super) pending_route: Option<String>,
    pub(super) pending_until: Option<Instant>,
    pub(super) route_intent_active: bool,
}

pub struct TuiModel {
    pub base_url: String,
    pub timeout: Duration,
    pub width: usize,
    pub height: usize,
    pub paused: bool,
    pub err: Option<String>,
    pub last_http_poll: Option<Instant>,
    pub(super) reboot_reconnect: Option<RebootReconnect>,
    pub status: String,
    pub history: std::collections::HashMap<String, Vec<i32>>,
    pub latest: std::collections::HashMap<String, AdcReading>,
    pub control_idx: usize,
    pub active_page: ActivePage,
    pub controls_scroll: usize,
    pub config_scroll: usize,
    pub status_scroll: usize,
    pub(super) power_names: Vec<String>,
    status_snapshot_received: bool,
    pub power_states: std::collections::HashMap<String, bool>,
    pub pd: Option<TuiPdStatus>,
    pub(super) pd_worker: PdWorker,
    pub(super) switches: BTreeMap<String, TuiSwitchState>,
    pub(super) hardware_confirm: Option<HardwareConfirmation>,
    pub(super) hit_map: HitMap,
    pub gpio_names: Vec<String>,
    pub gpio_notes: std::collections::HashMap<String, String>,
    pub gpio_levels: std::collections::HashMap<String, bool>,
    pub gpio_is_input: std::collections::HashMap<String, bool>,
    pub(super) gpio_layouts: std::collections::HashMap<String, TuiGpioLayout>,
    pub(super) gpio_layout_groups: std::collections::HashMap<String, TuiGpioLayoutGroup>,
    pub monitoring: BoardMonitoring,
    pub(super) saved_config: SavedConfigState,
    pub(super) config_worker: ConfigWorker,
    pub(super) gpio_worker: GpioWorker,
    pub(super) gpio_pending: Option<GpioJob>,
    pub(super) gpio_gesture: super::gpio_gesture::GpioGesture,
    pub(super) gpio_poll_defer_until: Option<Instant>,
    pub closed: bool,
    pub channel_ids: Vec<String>,
    pub voltage_channel_ids: Vec<String>,
    pub(super) adc_channel_metadata: std::collections::HashMap<String, TuiStatusAdcChannel>,
}

impl TuiModel {
    pub(super) fn new(base_url: String, timeout: Duration) -> Self {
        Self {
            base_url: base_url.clone(),
            timeout,
            width: 0,
            height: 0,
            paused: false,
            err: None,
            last_http_poll: None,
            reboot_reconnect: None,
            status: "HTTP mode".to_string(),
            history: std::collections::HashMap::new(),
            latest: std::collections::HashMap::new(),
            control_idx: 0,
            active_page: ActivePage::default(),
            controls_scroll: 0,
            config_scroll: 0,
            status_scroll: 0,
            power_names: Vec::new(),
            status_snapshot_received: false,
            power_states: std::collections::HashMap::new(),
            pd: None,
            pd_worker: PdWorker::default(),
            switches: BTreeMap::new(),
            hardware_confirm: None,
            hit_map: HitMap::default(),
            gpio_names: Vec::new(),
            gpio_notes: std::collections::HashMap::new(),
            gpio_levels: std::collections::HashMap::new(),
            gpio_is_input: std::collections::HashMap::new(),
            gpio_layouts: std::collections::HashMap::new(),
            gpio_layout_groups: std::collections::HashMap::new(),
            monitoring: BoardMonitoring::default(),
            saved_config: SavedConfigState::default(),
            config_worker: ConfigWorker::new(),
            gpio_worker: GpioWorker::new(),
            gpio_pending: None,
            gpio_gesture: super::gpio_gesture::GpioGesture::default(),
            gpio_poll_defer_until: None,
            closed: false,
            channel_ids: Vec::new(),
            voltage_channel_ids: Vec::new(),
            adc_channel_metadata: std::collections::HashMap::new(),
        }
    }

    pub(super) fn apply_status_snapshot(&mut self, snapshot: WsStatusSnapshot) -> bool {
        let selected = self
            .status_snapshot_received
            .then(|| control_items(self).get(self.control_idx).cloned())
            .flatten();
        let config_changed = self.saved_config.observe_summary(snapshot.config);
        self.power_names.clear();
        self.power_states.clear();
        for output in snapshot.power_outputs {
            self.power_names.push(output.name.clone());
            self.power_states.insert(
                output.name.clone(),
                output.value != 0 || output.state == "on",
            );
        }
        self.apply_pd_status(snapshot.pd);
        self.channel_ids = snapshot
            .adc_channels
            .iter()
            .filter(|channel| channel.sensor_channel == "current")
            .map(|channel| channel.name.clone())
            .collect();
        self.voltage_channel_ids = snapshot
            .adc_channels
            .iter()
            .filter(|channel| channel.sensor_channel == "voltage")
            .map(|channel| channel.name.clone())
            .collect();
        self.adc_channel_metadata = snapshot
            .adc_channels
            .into_iter()
            .map(|channel| (channel.name.clone(), channel))
            .collect();
        let now = Instant::now();
        let mut switches = BTreeMap::new();
        for (name, info) in snapshot.switches {
            let previous = self.switches.remove(&name);
            let (desired_route, pending_route, pending_until, route_intent_active) = match previous
            {
                Some(previous)
                    if previous.route_intent_active && previous.desired_route == info.route =>
                {
                    (info.route.clone(), None, None, false)
                }
                Some(previous) if previous.route_intent_active => {
                    let is_pending = previous.pending_until.is_some_and(|until| now < until);
                    (
                        previous.desired_route,
                        if is_pending {
                            previous.pending_route
                        } else {
                            None
                        },
                        if is_pending {
                            previous.pending_until
                        } else {
                            None
                        },
                        true,
                    )
                }
                Some(_) | None => (info.route.clone(), None, None, false),
            };
            switches.insert(
                name.clone(),
                TuiSwitchState {
                    name,
                    desired_route,
                    actual_route: info.route,
                    routes: info.routes,
                    pending_route,
                    pending_until,
                    route_intent_active,
                },
            );
        }
        self.switches = switches;
        if self
            .hardware_confirm
            .as_ref()
            .is_some_and(|confirm| !confirm.remains_advertised(&self.power_names, &self.switches))
        {
            self.hardware_confirm = None;
        }
        self.gpio_names = snapshot
            .gpios
            .iter()
            .map(|gpio| gpio.name.clone())
            .collect();
        self.gpio_gesture.cancel_missing_pin(&self.gpio_names);
        self.gpio_notes.clear();
        self.gpio_levels.clear();
        self.gpio_is_input.clear();
        self.gpio_layouts.clear();
        self.gpio_layout_groups = snapshot
            .gpio_layout_groups
            .into_iter()
            .map(|group| {
                (
                    group.name,
                    TuiGpioLayoutGroup {
                        edge: group.edge,
                        order: group.order,
                    },
                )
            })
            .collect();
        for gpio in &snapshot.gpios {
            self.gpio_notes.insert(gpio.name.clone(), gpio.note.clone());
            self.gpio_levels
                .insert(gpio.name.clone(), gpio.value.unwrap_or(0) != 0);
            self.gpio_is_input
                .insert(gpio.name.clone(), gpio.direction == "input");
            self.gpio_layouts.insert(
                gpio.name.clone(),
                TuiGpioLayout {
                    pin: gpio.pin,
                    group: non_empty(&gpio.layout_group),
                    label: non_empty(&gpio.layout_label),
                    row: gpio.layout_row,
                    column: gpio.layout_column,
                },
            );
        }
        self.monitoring = snapshot.board_monitoring;
        let items = control_items(self);
        self.control_idx = selected
            .and_then(|selected| items.iter().position(|item| item == &selected))
            .unwrap_or_else(|| self.control_idx.min(items.len().saturating_sub(1)));
        self.status_snapshot_received = true;
        config_changed
    }

    pub(super) fn apply_adc_response(&mut self, readings: Vec<AdcReading>) {
        for reading in readings {
            if reading.kind == AdcKind::Current {
                if let Some(power_enabled) = reading.power_enabled {
                    self.power_states
                        .insert(reading.name.clone(), power_enabled);
                }
                let ma = current_milliamp_estimate(&reading);
                let series = self.history.entry(reading.name.clone()).or_default();
                series.push(ma);
                if series.len() > TUI_HISTORY_LIMIT {
                    let drain = series.len() - TUI_HISTORY_LIMIT;
                    series.drain(0..drain);
                }
            }
            self.latest.insert(reading.name.clone(), reading);
        }
    }

    pub(super) fn apply_pd_status(&mut self, mut pd: Option<TuiPdStatus>) {
        if !pd.as_ref().is_some_and(|pd| pd.available && pd.pd) {
            self.pd_worker.reset();
            if let Some(pd) = pd.as_mut() {
                pd.capabilities_valid = false;
                pd.fixed_profiles.clear();
                pd.pps_ranges.clear();
                pd.avs_ranges.clear();
            }
        }
        self.pd = pd;
    }

    pub(super) fn apply_action_msg(&mut self, msg: TuiActionMsg) {
        self.err = msg.err.clone();
        if let Some(err) = msg.err {
            self.status = err;
        } else {
            self.status = msg.status;
        }
    }

    pub(super) fn begin_reboot_reconnect(&mut self, accepted_at: Instant) {
        self.last_http_poll = Some(accepted_at);
        self.reboot_reconnect = Some(RebootReconnect {
            accepted_at,
            deadline: accepted_at + REBOOT_RECONNECT_TIMEOUT,
        });
    }

    pub(super) fn clear_reboot_reconnect(&mut self) {
        self.reboot_reconnect = None;
    }
}

fn non_empty(value: &Option<String>) -> Option<String> {
    value.as_ref().filter(|text| !text.is_empty()).cloned()
}

pub(super) fn current_milliamp_estimate(reading: &AdcReading) -> i32 {
    match reading.kind {
        crate::adc::AdcKind::Current => {
            if let Some(ma_est) = reading.ma_est {
                return ma_est;
            }
            if let Some(current_ua) = reading.current_ua {
                return current_ua / 1000;
            }
            if let Some(sensor_value) = &reading.sensor_value {
                return (sensor_value.val1 * 1_000_000 + sensor_value.val2) / 1000;
            }
            0
        }
        crate::adc::AdcKind::Voltage => 0,
    }
}
