// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Deserialize, Serialize, Default)]
pub struct MonitoringAvailability {
    pub available: bool,
    #[serde(default)]
    pub reason: String,
    pub error: Option<i32>,
}

#[derive(Debug, Clone, Deserialize, Serialize, Default)]
pub struct MonitoringSensorValue {
    pub val1: i32,
    pub val2: i32,
}

#[derive(Debug, Clone, Deserialize, Serialize, Default)]
pub struct MonitoringTemperature {
    #[serde(flatten)]
    pub availability: MonitoringAvailability,
    #[serde(default)]
    pub source: String,
    pub celsius: Option<MonitoringSensorValue>,
}

#[derive(Debug, Clone, Deserialize, Serialize, Default)]
pub struct MonitoringHeap {
    #[serde(flatten)]
    pub availability: MonitoringAvailability,
    #[serde(default)]
    pub source: String,
    pub free_bytes: Option<u32>,
    pub allocated_bytes: Option<u32>,
    pub max_allocated_bytes: Option<u32>,
    pub total_bytes: Option<u32>,
}

#[derive(Debug, Clone, Deserialize, Serialize, Default)]
pub struct MonitoringRuntime {
    #[serde(flatten)]
    pub availability: MonitoringAvailability,
    pub uptime_ms: Option<i64>,
    pub uptime_seconds: Option<u64>,
}

#[derive(Debug, Clone, Deserialize, Serialize, Default)]
pub struct MonitoringCpu {
    #[serde(flatten)]
    pub availability: MonitoringAvailability,
    pub active_pct_x100: Option<u32>,
    pub window_ms: Option<u32>,
    pub busy_cycles_delta: Option<u64>,
    pub total_cycles_delta: Option<u64>,
}

#[derive(Debug, Clone, Deserialize, Serialize, Default)]
pub struct BoardMonitoring {
    #[serde(default)]
    pub temperature: MonitoringTemperature,
    #[serde(default)]
    pub heap: MonitoringHeap,
    #[serde(default)]
    pub runtime: MonitoringRuntime,
    #[serde(default)]
    pub cpu: MonitoringCpu,
}

pub fn format_monitoring_summary(m: &BoardMonitoring) -> String {
    format!(
        "board: temp {} · heap {} · runtime {} · cpu {}",
        format_temperature_summary(&m.temperature),
        format_heap_summary(&m.heap),
        format_runtime_summary(&m.runtime),
        format_cpu_summary(&m.cpu)
    )
}

fn format_temperature_summary(temp: &MonitoringTemperature) -> String {
    if !temp.availability.available {
        return unavailable_text(&temp.availability.reason);
    }
    if let Some(celsius) = &temp.celsius {
        return format!(
            "{:.2}°C",
            f64::from(celsius.val1) + f64::from(celsius.val2) / 1_000_000.0
        );
    }
    unavailable_text("missing_temperature_value")
}

fn format_heap_summary(heap: &MonitoringHeap) -> String {
    if !heap.availability.available {
        return unavailable_text(&heap.availability.reason);
    }
    match (
        heap.allocated_bytes,
        heap.total_bytes.or_else(|| {
            heap.allocated_bytes
                .zip(heap.free_bytes)
                .map(|(allocated, free)| allocated + free)
        }),
    ) {
        (Some(allocated), Some(total)) => format!("{allocated}/{total}B"),
        _ => unavailable_text("missing_heap_values"),
    }
}

fn format_runtime_summary(runtime: &MonitoringRuntime) -> String {
    if !runtime.availability.available {
        return unavailable_text(&runtime.availability.reason);
    }
    runtime
        .uptime_seconds
        .map(|seconds| format!("{seconds}s uptime"))
        .unwrap_or_else(|| unavailable_text("missing_runtime_values"))
}

fn format_cpu_summary(cpu: &MonitoringCpu) -> String {
    if !cpu.availability.available {
        return unavailable_text(&cpu.availability.reason);
    }
    match (cpu.active_pct_x100, cpu.window_ms) {
        (Some(active), Some(window)) => format!("{:.2}%/{window}ms", f64::from(active) / 100.0),
        _ => unavailable_text("missing_cpu_values"),
    }
}

fn unavailable_text(reason: &str) -> String {
    if reason.is_empty() {
        return "n/a(unavailable)".to_string();
    }
    format!("n/a({reason})")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn formats_monitoring_summary() {
        let monitoring = BoardMonitoring {
            temperature: MonitoringTemperature {
                availability: MonitoringAvailability {
                    available: false,
                    reason: "no_zephyr_temperature_device".to_string(),
                    error: None,
                },
                ..Default::default()
            },
            heap: MonitoringHeap {
                availability: MonitoringAvailability {
                    available: true,
                    reason: String::new(),
                    error: None,
                },
                allocated_bytes: Some(2048),
                free_bytes: Some(1024),
                total_bytes: Some(3072),
                ..Default::default()
            },
            ..Default::default()
        };

        let summary = format_monitoring_summary(&monitoring);
        assert!(summary.contains("n/a(no_zephyr_temperature_device)"));
        assert!(summary.contains("2048/3072B"));
    }
}
