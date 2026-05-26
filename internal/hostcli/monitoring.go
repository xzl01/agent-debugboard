// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

package hostcli

import "fmt"

type monitoringAvailability struct {
	Available bool   `json:"available"`
	Reason    string `json:"reason"`
	Error     *int   `json:"error,omitempty"`
}

type monitoringSensorValue struct {
	Val1 int32 `json:"val1"`
	Val2 int32 `json:"val2"`
}

type monitoringTemperature struct {
	monitoringAvailability
	Source  string                 `json:"source,omitempty"`
	Celsius *monitoringSensorValue `json:"celsius,omitempty"`
}

type monitoringHeap struct {
	monitoringAvailability
	Source            string `json:"source,omitempty"`
	FreeBytes         uint32 `json:"free_bytes,omitempty"`
	AllocatedBytes    uint32 `json:"allocated_bytes,omitempty"`
	MaxAllocatedBytes uint32 `json:"max_allocated_bytes,omitempty"`
	TotalBytes        uint32 `json:"total_bytes,omitempty"`
}

type monitoringRuntime struct {
	monitoringAvailability
	UptimeMS      int64  `json:"uptime_ms,omitempty"`
	UptimeSeconds uint64 `json:"uptime_seconds,omitempty"`
}

type monitoringCPU struct {
	monitoringAvailability
	ActivePctX100    uint32 `json:"active_pct_x100,omitempty"`
	WindowMS         uint32 `json:"window_ms,omitempty"`
	BusyCyclesDelta  uint64 `json:"busy_cycles_delta,omitempty"`
	TotalCyclesDelta uint64 `json:"total_cycles_delta,omitempty"`
}

type boardMonitoring struct {
	Temperature monitoringTemperature `json:"temperature"`
	Heap        monitoringHeap        `json:"heap"`
	Runtime     monitoringRuntime     `json:"runtime"`
	CPU         monitoringCPU         `json:"cpu"`
}

func formatMonitoringSummary(m boardMonitoring) string {
	return fmt.Sprintf("board: temp %s · heap %s · runtime %s · cpu %s",
		formatTemperatureSummary(m.Temperature),
		formatHeapSummary(m.Heap),
		formatRuntimeSummary(m.Runtime),
		formatCPUSummary(m.CPU))
}

func formatTemperatureSummary(temp monitoringTemperature) string {
	if !temp.Available {
		return unavailableText(temp.Reason)
	}
	if temp.Celsius == nil {
		return unavailableText("missing_temperature_value")
	}
	return fmt.Sprintf("%.2f°C", float64(temp.Celsius.Val1)+float64(temp.Celsius.Val2)/1000000.0)
}

func formatHeapSummary(heap monitoringHeap) string {
	if !heap.Available {
		return unavailableText(heap.Reason)
	}
	total := heap.TotalBytes
	if total == 0 {
		total = heap.AllocatedBytes + heap.FreeBytes
	}
	return fmt.Sprintf("%d/%dB", heap.AllocatedBytes, total)
}

func formatRuntimeSummary(runtime monitoringRuntime) string {
	if !runtime.Available {
		return unavailableText(runtime.Reason)
	}
	return fmt.Sprintf("%ds uptime", runtime.UptimeSeconds)
}

func formatCPUSummary(cpu monitoringCPU) string {
	if !cpu.Available {
		return unavailableText(cpu.Reason)
	}
	return fmt.Sprintf("%.2f%%/%dms", float64(cpu.ActivePctX100)/100.0, cpu.WindowMS)
}

func unavailableText(reason string) string {
	if reason == "" {
		reason = "unavailable"
	}
	return "n/a(" + reason + ")"
}
