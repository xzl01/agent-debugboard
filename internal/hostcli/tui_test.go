// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

package hostcli

import (
	"strings"
	"testing"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/gorilla/websocket"
)

func TestTUITelemetryUpdatesPowerStateFromADCReading(t *testing.T) {
	enabled := true
	model := tuiModel{
		timeout:     2 * time.Second,
		status:      "Connecting…",
		history:     map[string][]int32{},
		latest:      map[string]adcReading{},
		powerStates: map[string]bool{"5v_out": false},
		sdRoute:     "target",
		channelIDs:  []string{"5v_out", "12v_out", "20v_out"},
		wsClient:    newTestWSClient(t, DefaultBaseURL),
	}

	updated, _ := model.Update(tuiStreamMsg{generation: model.wsClient.Generation(), telemetry: &adcResponse{
		Readings: []adcReading{{
			Name:         "5v_out",
			PowerEnabled: &enabled,
			MAEst:        int32Ptr(500),
			CurrentValid: boolPtr(true),
		}},
	}})

	next := updated.(tuiModel)
	if !next.powerStates["5v_out"] {
		t.Fatalf("power state was not updated from adc reading")
	}
	if got := currentMilliampEstimate(next.latest["5v_out"]); got != 500 {
		t.Fatalf("currentMilliampEstimate = %d", got)
	}
}

func TestTUIStatusSnapshotUpdatesBoardMonitoring(t *testing.T) {
	model := tuiModel{
		status:      "Connecting…",
		history:     map[string][]int32{},
		latest:      map[string]adcReading{},
		powerStates: map[string]bool{},
		sdRoute:     "target",
		channelIDs:  []string{"5v_out", "12v_out", "20v_out"},
		wsClient:    newTestWSClient(t, DefaultBaseURL),
	}
	snapshot := &wsStatusSnapshot{
		PowerOutputs: []tuiStatusPowerOutput{{Name: "5v_out", State: "on", Value: 1}},
		Switches:     tuiStatusSwitches{SD: tuiStatusSwitchRoute{Route: "usb-reader"}},
		BoardMonitoring: boardMonitoring{
			Temperature: monitoringTemperature{
				monitoringAvailability: monitoringAvailability{Available: false, Reason: "no_zephyr_temperature_device"},
			},
			Heap: monitoringHeap{
				monitoringAvailability: monitoringAvailability{Available: true},
				FreeBytes:              1024,
				AllocatedBytes:         2048,
				TotalBytes:             3072,
			},
			Runtime: monitoringRuntime{
				monitoringAvailability: monitoringAvailability{Available: true},
				UptimeMS:               12345,
				UptimeSeconds:          12,
			},
			CPU: monitoringCPU{
				monitoringAvailability: monitoringAvailability{Available: false, Reason: "thread_runtime_stats_disabled"},
			},
		},
	}

	updated, _ := model.Update(tuiStreamMsg{generation: model.wsClient.Generation(), snapshot: snapshot})
	next := updated.(tuiModel)
	if !next.powerStates["5v_out"] || next.sdRoute != "usb-reader" {
		t.Fatalf("snapshot state not applied: power=%v sd=%s", next.powerStates, next.sdRoute)
	}
	if got := formatMonitoringSummary(next.monitoring); !strings.Contains(got, "n/a(no_zephyr_temperature_device)") || !strings.Contains(got, "2048/3072B") {
		t.Fatalf("monitoring summary = %q", got)
	}
}

func TestTUITimeTickOnlySchedulesNextTickWhenConnected(t *testing.T) {
	model := tuiModel{
		app:         App{},
		baseURL:     DefaultBaseURL,
		timeout:     2 * time.Second,
		status:      "Live",
		history:     map[string][]int32{},
		latest:      map[string]adcReading{},
		powerStates: map[string]bool{},
		sdRoute:     "target",
		channelIDs:  []string{"5v_out", "12v_out", "20v_out"},
		wsClient:    &WSClient{baseURL: DefaultBaseURL, conn: &websocket.Conn{}},
	}

	updated, cmd := model.Update(time.Now())
	if updated == nil {
		t.Fatal("expected updated model")
	}
	if cmd == nil {
		t.Fatal("expected tick to schedule follow-up commands")
	}

	msg := cmd()
	batch, ok := msg.(tea.BatchMsg)
	if !ok {
		t.Fatalf("expected tea.BatchMsg, got %T", msg)
	}
	if len(batch) != 2 {
		t.Fatalf("expected 2 batched commands when websocket is connected, got %d", len(batch))
	}
}

func TestTUITimeTickReconnectsWhenWebsocketDisconnected(t *testing.T) {
	model := tuiModel{
		app:         App{},
		baseURL:     DefaultBaseURL,
		timeout:     2 * time.Second,
		status:      "Live",
		history:     map[string][]int32{},
		latest:      map[string]adcReading{},
		powerStates: map[string]bool{},
		sdRoute:     "target",
		channelIDs:  []string{"5v_out", "12v_out", "20v_out"},
		wsClient:    newTestWSClient(t, DefaultBaseURL),
	}

	updated, cmd := model.Update(time.Now())
	if updated == nil {
		t.Fatal("expected updated model")
	}
	if cmd == nil {
		t.Fatal("expected tick to schedule follow-up commands")
	}

	msg := cmd()
	batch, ok := msg.(tea.BatchMsg)
	if !ok {
		t.Fatalf("expected tea.BatchMsg, got %T", msg)
	}
	if len(batch) != 2 {
		t.Fatalf("expected 2 batched commands when websocket is disconnected, got %d", len(batch))
	}
}

func TestTUIQuitClosesWebsocketAndStopsPendingState(t *testing.T) {
	client := newTestWSClient(t, DefaultBaseURL)
	before := client.Generation()
	model := tuiModel{
		app:            App{},
		baseURL:        DefaultBaseURL,
		timeout:        2 * time.Second,
		status:         "Live",
		history:        map[string][]int32{},
		latest:         map[string]adcReading{},
		powerStates:    map[string]bool{},
		sdRoute:        "target",
		channelIDs:     []string{"5v_out", "12v_out", "20v_out"},
		connectPending: true,
		streamPending:  true,
		wsClient:       client,
	}

	updated, cmd := model.Update(tea.KeyMsg{Type: tea.KeyCtrlC})
	next := updated.(tuiModel)
	if cmd == nil {
		t.Fatal("expected quit command")
	}
	if !next.closed {
		t.Fatal("expected model to be closed")
	}
	if next.connectPending || next.streamPending {
		t.Fatalf("pending flags should be cleared: connect=%v stream=%v", next.connectPending, next.streamPending)
	}
	if client.Generation() <= before {
		t.Fatalf("expected websocket client generation to advance on quit")
	}
	if client.IsCurrentGeneration(before) {
		t.Fatalf("old generation %d should be stale after quit", before)
	}
	if _, ok := cmd().(tea.QuitMsg); !ok {
		t.Fatalf("expected tea.QuitMsg, got %T", cmd())
	}
	updated, followup := next.Update(time.Now())
	if followup != nil {
		t.Fatalf("closed model should not schedule follow-up work")
	}
	if !updated.(tuiModel).closed {
		t.Fatal("closed model should stay closed")
	}
}

func TestTUIIgnoresStaleStreamMessageGeneration(t *testing.T) {
	client := newTestWSClient(t, DefaultBaseURL)
	current := client.Generation()
	model := tuiModel{
		status:      "Live",
		history:     map[string][]int32{},
		latest:      map[string]adcReading{},
		powerStates: map[string]bool{},
		sdRoute:     "target",
		channelIDs:  []string{"5v_out", "12v_out", "20v_out"},
		wsClient:    client,
	}

	updated, cmd := model.Update(tuiStreamMsg{
		generation: current + 1,
		snapshot: &wsStatusSnapshot{
			PowerOutputs: []tuiStatusPowerOutput{{Name: "5v_out", State: "on", Value: 1}},
		},
	})
	next := updated.(tuiModel)
	if next.powerStates["5v_out"] {
		t.Fatal("stale stream message should be ignored")
	}
	if cmd != nil {
		t.Fatalf("stale stream message should not schedule follow-up work")
	}
}

func TestRenderBarChartHeight4(t *testing.T) {
	// Verify the chart renders without panic at the new default height.
	_ = renderBarChart("5v_out", "0.50A", "500mA", "on", []int32{100, 200, 500}, 500, 26, 4, 5000)
}

func TestRenderBarChartContinuousFill(t *testing.T) {
	// At 500 mA / 5000 mA max with height=4, sqrt(0.1)≈0.316, so the chart
	// should use sub-row braille cells instead of only empty/full rows.
	result := renderBarChart("5v_out", "0.50A", "500mA", "on", []int32{100, 200, 500}, 500, 26, 4, 5000)
	hasBraille := strings.Contains(result, "⣀") || strings.Contains(result, "⣤") || strings.Contains(result, "⣶")
	if !hasBraille {
		t.Fatalf("expected a braille-style subcell rune in chart output for 500mA/5A, got:\n%s", result)
	}
	if !strings.Contains(result, "0.50A") {
		t.Fatalf("expected realtime current text in chart output, got:\n%s", result)
	}
}
