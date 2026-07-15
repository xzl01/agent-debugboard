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
pub struct MonitoringMemoryPhysical {
    pub total_bytes: Option<u64>,
    pub image_reserved_bytes: Option<u64>,
    pub reserved_pct_x100: Option<u32>,
}

#[derive(Debug, Clone, Deserialize, Serialize, Default)]
pub struct MonitoringMemoryStacks {
    pub thread_count: Option<u32>,
    pub measured_count: Option<u32>,
    pub error_count: Option<u32>,
    pub total_bytes: Option<u64>,
    pub used_high_water_bytes: Option<u64>,
    pub max_pressure_pct_x100: Option<u32>,
    #[serde(default)]
    pub max_pressure_thread: String,
}

#[derive(Debug, Clone, Deserialize, Serialize, Default)]
pub struct MonitoringMemoryPressureSummary {
    #[serde(flatten)]
    pub availability: MonitoringAvailability,
    #[serde(default)]
    pub coverage: String,
    pub pressure_pct_x100: Option<u32>,
    #[serde(default)]
    pub limiting_component: String,
    #[serde(default)]
    pub limiting_name: String,
    pub tie_count: Option<u32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub since: Option<String>,
}

#[derive(Debug, Clone, Deserialize, Serialize, Default)]
pub struct MonitoringMemory {
    #[serde(flatten)]
    pub availability: MonitoringAvailability,
    #[serde(default)]
    pub source: String,
    #[serde(default)]
    pub coverage: String,
    pub pressure_pct_x100: Option<u32>,
    #[serde(default)]
    pub limiting_component: String,
    #[serde(default)]
    pub limiting_name: String,
    pub system_heap_pressure_pct_x100: Option<u32>,
    #[serde(default)]
    #[serde(skip_serializing_if = "Option::is_none")]
    pub current_pressure: Option<MonitoringMemoryPressureSummary>,
    #[serde(default)]
    #[serde(skip_serializing_if = "Option::is_none")]
    pub peak_pressure: Option<MonitoringMemoryPressureSummary>,
    #[serde(default)]
    pub physical: MonitoringMemoryPhysical,
    #[serde(default)]
    pub stacks: MonitoringMemoryStacks,
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
    pub memory: Option<MonitoringMemory>,
    #[serde(default)]
    pub runtime: MonitoringRuntime,
    #[serde(default)]
    pub cpu: MonitoringCpu,
}

pub fn format_monitoring_summary(m: &BoardMonitoring) -> String {
    let memory_label = if let Some(memory) = &m.memory {
        format!("mem {}", format_memory_summary(memory))
    } else {
        format!("heap {}", format_heap_summary(&m.heap))
    };

    format!(
        "board: temp {} · {} · runtime {} · cpu {}",
        format_temperature_summary(&m.temperature),
        memory_label,
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

fn format_memory_summary(memory: &MonitoringMemory) -> String {
    if let Some(current) = memory_phase2_current_pressure(memory) {
        return format_memory_pressure_summary(memory, current);
    }

    if !memory.availability.available {
        return unavailable_text(&memory.availability.reason);
    }

    let Some(current) = memory_legacy_current_pressure(memory) else {
        return unavailable_text("missing_memory_pressure");
    };

    format_memory_pressure_summary(memory, current)
}

fn format_memory_pressure_summary(
    memory: &MonitoringMemory,
    current: DisplayMemoryPressure,
) -> String {
    let mut summary = format_pressure_summary(&current);
    if let Some(peak) = memory_peak_pressure(memory, &current) {
        summary.push_str(" peak ");
        summary.push_str(&format_pressure_summary(&peak));
    }
    summary
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct DisplayMemoryPressure {
    pressure_pct_x100: u32,
    limiting_component: String,
    limiting_name: String,
}

fn memory_phase2_current_pressure(memory: &MonitoringMemory) -> Option<DisplayMemoryPressure> {
    memory
        .current_pressure
        .as_ref()
        .and_then(pressure_summary_value)
}

fn memory_legacy_current_pressure(memory: &MonitoringMemory) -> Option<DisplayMemoryPressure> {
    memory
        .pressure_pct_x100
        .map(|pressure_pct_x100| DisplayMemoryPressure {
            pressure_pct_x100,
            limiting_component: memory.limiting_component.clone(),
            limiting_name: memory.limiting_name.clone(),
        })
}

fn pressure_summary_value(
    pressure: &MonitoringMemoryPressureSummary,
) -> Option<DisplayMemoryPressure> {
    if !pressure.availability.available {
        return None;
    }
    pressure
        .pressure_pct_x100
        .map(|pressure_pct_x100| DisplayMemoryPressure {
            pressure_pct_x100,
            limiting_component: pressure.limiting_component.clone(),
            limiting_name: pressure.limiting_name.clone(),
        })
}

fn memory_peak_pressure(
    memory: &MonitoringMemory,
    current: &DisplayMemoryPressure,
) -> Option<DisplayMemoryPressure> {
    let peak = memory.peak_pressure.as_ref()?;
    let peak_value = pressure_summary_value(peak)?;
    if peak_value != *current || peak_value.limiting_component == "thread_stack" {
        Some(peak_value)
    } else {
        None
    }
}

fn format_pressure_summary(pressure: &DisplayMemoryPressure) -> String {
    let mut summary = format_pct_x100(pressure.pressure_pct_x100);
    if let Some(limiter) = memory_limiter(&pressure.limiting_component, &pressure.limiting_name) {
        summary.push(' ');
        summary.push_str(&limiter);
    }
    summary
}

fn memory_limiter(component: &str, name: &str) -> Option<String> {
    match (display_memory_component(component), name) {
        ("", "") => None,
        (component, "") => Some(component.to_string()),
        ("", name) => Some(name.to_string()),
        (component, name) => Some(format!("{component}/{name}")),
    }
}

fn display_memory_component(component: &str) -> &str {
    match component {
        "system_heap" => "heap",
        "net_pkt_rx" => "pkt-rx",
        "net_pkt_tx" => "pkt-tx",
        "net_buf_rx_data" => "buf-rx",
        "net_buf_tx_data" => "buf-tx",
        "thread_stack" => "stack",
        other => other,
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

fn format_pct_x100(value: u32) -> String {
    format!("{:.2}%", f64::from(value) / 100.0)
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

    #[test]
    fn parses_full_memory_monitoring_shape() {
        let monitoring: BoardMonitoring = serde_json::from_str(
            r#"{
                "memory": {
                    "available": true,
                    "reason": "",
                    "source": "zephyr",
                    "coverage": "heap_and_stacks",
                    "pressure_pct_x100": 4250,
                    "limiting_component": "thread_stack",
                    "limiting_name": "main",
                    "system_heap_pressure_pct_x100": 3000,
                    "current_pressure": {
                        "available": true,
                        "reason": "",
                        "coverage": "heap_and_stacks",
                        "pressure_pct_x100": 3000,
                        "limiting_component": "system_heap",
                        "limiting_name": "",
                        "tie_count": 1
                    },
                    "peak_pressure": {
                        "available": true,
                        "reason": "",
                        "coverage": "heap_and_stacks",
                        "pressure_pct_x100": 4250,
                        "limiting_component": "thread_stack",
                        "limiting_name": "main",
                        "tie_count": 1,
                        "since": "boot"
                    },
                    "physical": {
                        "total_bytes": 270336,
                        "image_reserved_bytes": 98304,
                        "reserved_pct_x100": 3721
                    },
                    "stacks": {
                        "thread_count": 7,
                        "measured_count": 6,
                        "error_count": 1,
                        "total_bytes": 12288,
                        "used_high_water_bytes": 4096,
                        "max_pressure_pct_x100": 4250,
                        "max_pressure_thread": "main"
                    }
                }
            }"#,
        )
        .unwrap();

        let memory = monitoring.memory.unwrap();
        assert!(memory.availability.available);
        assert_eq!(memory.source, "zephyr");
        assert_eq!(memory.coverage, "heap_and_stacks");
        assert_eq!(memory.pressure_pct_x100, Some(4250));
        assert_eq!(memory.limiting_component, "thread_stack");
        let current = memory.current_pressure.unwrap();
        assert_eq!(current.pressure_pct_x100, Some(3000));
        assert_eq!(current.limiting_component, "system_heap");
        assert_eq!(current.tie_count, Some(1));
        let peak = memory.peak_pressure.unwrap();
        assert_eq!(peak.pressure_pct_x100, Some(4250));
        assert_eq!(peak.limiting_component, "thread_stack");
        assert_eq!(peak.since.as_deref(), Some("boot"));
        assert_eq!(memory.physical.total_bytes, Some(270336));
        assert_eq!(memory.stacks.max_pressure_thread, "main");
    }

    #[test]
    fn formats_phase2_current_pressure_with_labeled_peak() {
        let monitoring = BoardMonitoring {
            memory: Some(MonitoringMemory {
                availability: MonitoringAvailability {
                    available: true,
                    reason: String::new(),
                    error: None,
                },
                pressure_pct_x100: Some(4250),
                limiting_component: "thread_stack".to_string(),
                limiting_name: "main".to_string(),
                current_pressure: Some(MonitoringMemoryPressureSummary {
                    availability: MonitoringAvailability {
                        available: true,
                        reason: String::new(),
                        error: None,
                    },
                    coverage: "heap_and_stacks".to_string(),
                    pressure_pct_x100: Some(3000),
                    limiting_component: "system_heap".to_string(),
                    tie_count: Some(1),
                    ..Default::default()
                }),
                peak_pressure: Some(MonitoringMemoryPressureSummary {
                    availability: MonitoringAvailability {
                        available: true,
                        reason: String::new(),
                        error: None,
                    },
                    coverage: "heap_and_stacks".to_string(),
                    pressure_pct_x100: Some(4250),
                    limiting_component: "thread_stack".to_string(),
                    limiting_name: "main".to_string(),
                    tie_count: Some(1),
                    ..Default::default()
                }),
                ..Default::default()
            }),
            ..Default::default()
        };

        let summary = format_monitoring_summary(&monitoring);
        assert!(summary.contains("mem 30.00% heap peak 42.50% stack/main"));
        assert!(!summary.contains("mem 42.50% stack/main ·"));
    }

    #[test]
    fn parses_absent_and_partial_memory_monitoring() {
        let old: BoardMonitoring = serde_json::from_str(r#"{"heap":{"available":true}}"#).unwrap();
        assert!(old.memory.is_none());

        let partial: BoardMonitoring = serde_json::from_str(
            r#"{"memory":{"available":true,"limiting_component":"system_heap"}}"#,
        )
        .unwrap();
        let memory = partial.memory.unwrap();
        assert!(memory.availability.available);
        assert_eq!(memory.pressure_pct_x100, None);
        assert_eq!(memory.limiting_component, "system_heap");
        assert_eq!(memory.physical.total_bytes, None);
    }

    #[test]
    fn formats_memory_summary_when_present_and_heap_when_absent() {
        let with_memory = BoardMonitoring {
            memory: Some(MonitoringMemory {
                availability: MonitoringAvailability {
                    available: true,
                    reason: String::new(),
                    error: None,
                },
                pressure_pct_x100: Some(4250),
                limiting_component: "thread_stack".to_string(),
                limiting_name: "main".to_string(),
                ..Default::default()
            }),
            heap: MonitoringHeap {
                availability: MonitoringAvailability {
                    available: true,
                    reason: String::new(),
                    error: None,
                },
                allocated_bytes: Some(2048),
                total_bytes: Some(3072),
                ..Default::default()
            },
            ..Default::default()
        };
        let summary = format_monitoring_summary(&with_memory);
        assert!(summary.contains("mem 42.50% stack/main"));
        assert!(!summary.contains("heap 2048/3072B"));

        let without_memory = BoardMonitoring {
            heap: with_memory.heap,
            ..Default::default()
        };
        assert!(format_monitoring_summary(&without_memory).contains("heap 2048/3072B"));
    }

    #[test]
    fn formats_known_memory_limiters_and_keeps_unknown_components() {
        let system_heap = MonitoringMemory {
            availability: MonitoringAvailability {
                available: true,
                reason: String::new(),
                error: None,
            },
            pressure_pct_x100: Some(1250),
            limiting_component: "system_heap".to_string(),
            ..Default::default()
        };
        assert_eq!(format_memory_summary(&system_heap), "12.50% heap");

        let unknown = MonitoringMemory {
            limiting_component: "custom_pool".to_string(),
            limiting_name: "dma".to_string(),
            ..system_heap
        };
        assert_eq!(format_memory_summary(&unknown), "12.50% custom_pool/dma");
    }

    #[test]
    fn unavailable_phase2_current_falls_back_to_phase1_pressure() {
        let memory = MonitoringMemory {
            availability: MonitoringAvailability {
                available: true,
                reason: String::new(),
                error: None,
            },
            pressure_pct_x100: Some(4250),
            limiting_component: "thread_stack".to_string(),
            limiting_name: "main".to_string(),
            current_pressure: Some(MonitoringMemoryPressureSummary {
                availability: MonitoringAvailability {
                    available: false,
                    reason: "insufficient_stack_coverage".to_string(),
                    error: None,
                },
                ..Default::default()
            }),
            ..Default::default()
        };

        assert_eq!(format_memory_summary(&memory), "42.50% stack/main");
    }

    #[test]
    fn valid_phase2_current_takes_priority_over_unavailable_legacy_root() {
        let memory = MonitoringMemory {
            availability: MonitoringAvailability {
                available: false,
                reason: "legacy_memory_unavailable".to_string(),
                error: None,
            },
            pressure_pct_x100: Some(4250),
            limiting_component: "thread_stack".to_string(),
            limiting_name: "main".to_string(),
            current_pressure: Some(MonitoringMemoryPressureSummary {
                availability: MonitoringAvailability {
                    available: true,
                    reason: String::new(),
                    error: None,
                },
                pressure_pct_x100: Some(3000),
                limiting_component: "system_heap".to_string(),
                ..Default::default()
            }),
            ..Default::default()
        };

        assert_eq!(format_memory_summary(&memory), "30.00% heap");
    }

    #[test]
    fn unavailable_phase2_current_preserves_unavailable_legacy_root() {
        let memory = MonitoringMemory {
            availability: MonitoringAvailability {
                available: false,
                reason: "legacy_memory_unavailable".to_string(),
                error: None,
            },
            pressure_pct_x100: Some(4250),
            limiting_component: "thread_stack".to_string(),
            limiting_name: "main".to_string(),
            current_pressure: Some(MonitoringMemoryPressureSummary {
                availability: MonitoringAvailability {
                    available: false,
                    reason: "current_unavailable".to_string(),
                    error: None,
                },
                pressure_pct_x100: Some(3000),
                limiting_component: "system_heap".to_string(),
                ..Default::default()
            }),
            ..Default::default()
        };

        assert_eq!(
            format_memory_summary(&memory),
            "n/a(legacy_memory_unavailable)"
        );
    }

    #[test]
    fn formats_phase2_memory_component_labels() {
        for (component, label) in [
            ("system_heap", "heap"),
            ("net_pkt_rx", "pkt-rx"),
            ("net_pkt_tx", "pkt-tx"),
            ("net_buf_rx_data", "buf-rx"),
            ("net_buf_tx_data", "buf-tx"),
            ("thread_stack", "stack"),
        ] {
            let memory = MonitoringMemory {
                availability: MonitoringAvailability {
                    available: true,
                    reason: String::new(),
                    error: None,
                },
                current_pressure: Some(MonitoringMemoryPressureSummary {
                    availability: MonitoringAvailability {
                        available: true,
                        reason: String::new(),
                        error: None,
                    },
                    pressure_pct_x100: Some(1250),
                    limiting_component: component.to_string(),
                    ..Default::default()
                }),
                ..Default::default()
            };
            assert_eq!(format_memory_summary(&memory), format!("12.50% {label}"));
        }
    }
}
