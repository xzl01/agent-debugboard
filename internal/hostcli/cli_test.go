// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) xzl <xiangzelong@radxa.com>
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

package hostcli

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

type fakeClient struct {
	requests []boardRequest
	response string
	err      error
}

func mustTestWSURL(t *testing.T, baseURL string) string {
	t.Helper()

	parsed, err := url.Parse(resolveBaseURL(baseURL))
	if err != nil {
		t.Fatalf("url.Parse() error = %v", err)
	}

	switch parsed.Scheme {
	case "http":
		parsed.Scheme = "ws"
	case "https":
		parsed.Scheme = "wss"
	default:
		t.Fatalf("unexpected scheme %q", parsed.Scheme)
	}

	parsed.Path = strings.TrimRight(parsed.Path, "/") + "/api/v1/ws/1"
	parsed.RawQuery = ""
	parsed.Fragment = ""
	return parsed.String()
}

func newTestWSClient(t *testing.T, baseURL string) *WSClient {
	t.Helper()
	return NewWSClientURL(baseURL, mustTestWSURL(t, baseURL))
}

func (c *fakeClient) Do(ctx context.Context, request boardRequest) ([]byte, error) {
	c.requests = append(c.requests, request)
	if c.err != nil {
		return nil, c.err
	}
	return []byte(c.response), nil
}

func TestCleanOutputStripsEchoPromptAndANSI(t *testing.T) {
	raw := "\r\ndebugboard status\r\nproject=radxa-linkr-debugger\r\n\x1b[1;32mlinkr-debugger:~$ \x1b[m\r\n"

	got := CleanOutput(raw, "debugboard status")
	if got != "project=radxa-linkr-debugger" {
		t.Fatalf("CleanOutput() = %q", got)
	}
}

func TestRunWithoutArgsStartsTUI(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	called := false

	app := App{
		RunTUI: func(baseURL string, timeout time.Duration, stdout io.Writer, stderr io.Writer) int {
			called = true
			if baseURL != DefaultBaseURL {
				t.Fatalf("baseURL = %q", baseURL)
			}
			if timeout != 2*time.Second {
				t.Fatalf("timeout = %s", timeout)
			}
			return 0
		},
	}

	code := app.Run(nil, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("Run() exit code = %d stderr=%q", code, stderr.String())
	}
	if !called {
		t.Fatal("expected RunTUI to be called")
	}
}

func TestRunStatusUsesHTTPClient(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	client := &fakeClient{response: `{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"status","project":"radxa-linkr-debugger"}`}

	code := (App{Client: client}).Run([]string{"status"}, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("Run() exit code = %d stderr=%q stdout=%q", code, stderr.String(), stdout.String())
	}
	if len(client.requests) != 1 || client.requests[0].Method != http.MethodGet || client.requests[0].Path != "/api/v1/status" {
		t.Fatalf("requests = %#v", client.requests)
	}
	if strings.TrimSpace(stdout.String()) != client.response {
		t.Fatalf("stdout = %q", stdout.String())
	}
}

func TestRunRawCommandIsRejectedOverHTTP(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer

	code := (App{Client: &fakeClient{}}).Run([]string{"--raw", "version"}, &stdout, &stderr)
	if code != 2 {
		t.Fatalf("Run() exit code = %d", code)
	}
	if !strings.Contains(stderr.String(), "raw shell commands are not available") {
		t.Fatalf("stderr = %q", stderr.String())
	}
}

func TestRunJSONCommandRequestsAndValidatesEnvelope(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	response := `{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"status","project":"radxa-linkr-debugger"}`
	client := &fakeClient{response: response}

	code := (App{Client: client}).Run([]string{"--json", "status"}, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("Run() exit code = %d stderr=%q stdout=%q", code, stderr.String(), stdout.String())
	}
	if strings.TrimSpace(stdout.String()) != response {
		t.Fatalf("stdout = %q", stdout.String())
	}
}

func TestRequestLiveSessionWSURL(t *testing.T) {
	client := &fakeClient{response: `{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"live-sessions","action":"create","session_id":7,"ws_url":"ws://172.29.203.1:8080/api/v1/ws/2"}`}
	wsURL, err := requestLiveSessionWSURL(App{Client: client}, 2*time.Second)
	if err != nil {
		t.Fatalf("requestLiveSessionWSURL() error = %v", err)
	}
	if wsURL != "ws://172.29.203.1:8080/api/v1/ws/2" {
		t.Fatalf("wsURL = %q", wsURL)
	}
	if len(client.requests) != 1 || client.requests[0].Method != http.MethodPost || client.requests[0].Path != "/api/v1/live-sessions" {
		t.Fatalf("requests = %#v", client.requests)
	}
}

func TestStatusJSONIncludesBoardMonitoringShape(t *testing.T) {
	response := `{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"status","project":"radxa-linkr-debugger","board_monitoring":{"temperature":{"available":false,"reason":"no_zephyr_temperature_device"},"heap":{"available":true,"reason":"","source":"system_heap","free_bytes":6144,"allocated_bytes":2048,"max_allocated_bytes":3072,"total_bytes":8192},"runtime":{"available":true,"reason":"","uptime_ms":12345,"uptime_seconds":12},"cpu":{"available":false,"reason":"thread_runtime_stats_disabled"}}}`
	var status struct {
		BoardMonitoring boardMonitoring `json:"board_monitoring"`
	}

	if err := json.Unmarshal([]byte(response), &status); err != nil {
		t.Fatalf("decode status: %v", err)
	}
	if status.BoardMonitoring.Temperature.Available {
		t.Fatalf("temperature should be unavailable: %#v", status.BoardMonitoring.Temperature)
	}
	if status.BoardMonitoring.Temperature.Reason != "no_zephyr_temperature_device" {
		t.Fatalf("temperature reason = %q", status.BoardMonitoring.Temperature.Reason)
	}
	if status.BoardMonitoring.Heap.Source != "system_heap" || status.BoardMonitoring.Heap.TotalBytes != 8192 {
		t.Fatalf("heap = %#v", status.BoardMonitoring.Heap)
	}
	if status.BoardMonitoring.Runtime.UptimeSeconds != 12 {
		t.Fatalf("runtime = %#v", status.BoardMonitoring.Runtime)
	}
}

func TestRunADCReadDefaultTextUsesRawCurrent(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	response := `{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","action":"read","readings":[{"name":"5v_out","signal":"S_C_5V","power_enabled":true,"sensor_channel":"current","unit":"A","sensor_value":{"val1":0,"val2":850000},"current_ua":850000}]}`
	client := &fakeClient{response: response}

	code := (App{Client: client}).Run([]string{"adc", "read", "5v_out"}, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("Run() exit code = %d stderr=%q stdout=%q", code, stderr.String(), stdout.String())
	}
	if len(client.requests) != 1 || client.requests[0].Path != "/api/v1/adc/read" || client.requests[0].Query.Get("channel") != "5v_out" {
		t.Fatalf("requests = %#v", client.requests)
	}
	if strings.TrimSpace(stdout.String()) != "5v_out=0.850000A" {
		t.Fatalf("stdout = %q", stdout.String())
	}
}

func TestRunADCReadJSONPreservesRawCurrentFields(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	response := `{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","action":"read","readings":[{"name":"5v_out","signal":"S_C_5V","power_enabled":true,"sensor_channel":"current","unit":"A","sensor_value":{"val1":0,"val2":850000},"current_ua":850000}]}`

	code := (App{Client: &fakeClient{response: response}}).Run([]string{"--json", "adc", "read", "5v_out"}, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("Run() exit code = %d stderr=%q stdout=%q", code, stderr.String(), stdout.String())
	}
	if strings.TrimSpace(stdout.String()) != `{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","action":"read","readings":[{"name":"5v_out","signal":"S_C_5V","raw":null,"power_enabled":true,"sensor_channel":"current","unit":"A","sensor_value":{"val1":0,"val2":850000},"current_ua":850000}]}` {
		t.Fatalf("stdout = %q", stdout.String())
	}
}

func TestRunADCReadPowerDisabledStillReportsRawCurrent(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	response := `{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","action":"read","readings":[{"name":"5v_out","signal":"S_C_5V","power_enabled":false,"raw":24,"mv":19,"sensor_channel":"current","unit":"A","sensor_value":{"val1":0,"val2":850000},"current_ua":850000}]}`

	code := (App{Client: &fakeClient{response: response}}).Run([]string{"adc", "read", "5v_out"}, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("Run() exit code = %d stderr=%q stdout=%q", code, stderr.String(), stdout.String())
	}
	if strings.TrimSpace(stdout.String()) != "5v_out=0.850000A" {
		t.Fatalf("stdout = %q", stdout.String())
	}
}

func TestRunPowerSetMapsToPowerEndpoint(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	client := &fakeClient{response: `{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"power","action":"set","power_output":{"name":"12v_out","state":"off"}}`}

	code := (App{Client: client}).Run([]string{"power", "set", "12v_out", "off"}, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("Run() exit code = %d stderr=%q stdout=%q", code, stderr.String(), stdout.String())
	}
	if len(client.requests) != 1 || client.requests[0].Method != http.MethodPut || client.requests[0].Path != "/api/v1/power/12v_out" {
		t.Fatalf("requests = %#v", client.requests)
	}
	body := client.requests[0].Body.(map[string]string)
	if body["state"] != "off" {
		t.Fatalf("body = %#v", body)
	}
}

func TestRunRejectsInternalPowerOutputWithoutBoardAccess(t *testing.T) {
	for _, args := range [][]string{
		{"power", "get", internalPowerOutput},
		{"power", "set", internalPowerOutput, "off"},
	} {
		var stdout bytes.Buffer
		var stderr bytes.Buffer
		client := &fakeClient{}

		code := (App{Client: client}).Run(args, &stdout, &stderr)
		if code != 2 {
			t.Fatalf("Run(%v) exit code = %d stderr=%q", args, code, stderr.String())
		}
		if len(client.requests) != 0 {
			t.Fatalf("Run(%v) unexpected requests = %#v", args, client.requests)
		}
		if !strings.Contains(stderr.String(), "is internal and unavailable through the CLI") {
			t.Fatalf("Run(%v) stderr = %q", args, stderr.String())
		}
	}
}

func TestRunStatusHidesInternalPowerOutput(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	client := &fakeClient{response: `{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"status","power_outputs":[{"name":"12v_out","state":"off"},{"name":"5v_ws","state":"on"}]}`}

	code := (App{Client: client}).Run([]string{"--json", "status"}, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("Run() exit code = %d stderr=%q", code, stderr.String())
	}
	var output struct {
		PowerOutputs []struct {
			Name string `json:"name"`
		} `json:"power_outputs"`
	}
	if err := json.Unmarshal(stdout.Bytes(), &output); err != nil {
		t.Fatalf("decode output: %v", err)
	}
	if len(output.PowerOutputs) != 1 || output.PowerOutputs[0].Name != "12v_out" {
		t.Fatalf("power_outputs = %#v", output.PowerOutputs)
	}
}

func TestFilterInternalPowerOutputHandlesNestedDoctorStatus(t *testing.T) {
	input := `{"schema":"radxa-linkr-debugger.v1","status":{"power_outputs":[{"name":"5v_ws"},{"name":"5v_out"}]}}`
	output, err := filterInternalPowerOutput(input)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(output, `"name":"5v_ws"`) || !strings.Contains(output, `"name":"5v_out"`) {
		t.Fatalf("filtered output = %s", output)
	}
}

func TestRunSDGPIOAndBootloaderMappings(t *testing.T) {
	tests := []struct {
		name   string
		args   []string
		method string
		path   string
	}{
		{name: "switch route", args: []string{"switch", "route", "sd", "usb-reader"}, method: http.MethodPut, path: "/api/v1/switch/sd"},
		{name: "gpio input", args: []string{"gpio", "input", "GP13"}, method: http.MethodPut, path: "/api/v1/gpio/GP13"},
		{name: "gpio set", args: []string{"gpio", "set", "GP13", "1"}, method: http.MethodPut, path: "/api/v1/gpio/GP13"},
		{name: "watchdog status", args: []string{"watchdog", "status"}, method: http.MethodGet, path: "/api/v1/watchdog"},
		{name: "bootloader", args: []string{"bootloader"}, method: http.MethodPost, path: "/api/v1/bootloader"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			var stdout bytes.Buffer
			var stderr bytes.Buffer
			client := &fakeClient{response: `{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"ok"}`}
			code := (App{Client: client}).Run(tt.args, &stdout, &stderr)
			if code != 0 {
				t.Fatalf("Run() exit code = %d stderr=%q stdout=%q", code, stderr.String(), stdout.String())
			}
			if len(client.requests) != 1 || client.requests[0].Method != tt.method || client.requests[0].Path != tt.path {
				t.Fatalf("requests = %#v", client.requests)
			}
		})
	}
}

func TestRunWatchdogFeedIsRejectedLocally(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	client := &fakeClient{response: `{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"ok"}`}

	code := (App{Client: client}).Run([]string{"watchdog", "feed"}, &stdout, &stderr)
	if code != 2 {
		t.Fatalf("Run() exit code = %d stderr=%q", code, stderr.String())
	}
	if len(client.requests) != 0 {
		t.Fatalf("unexpected requests = %#v", client.requests)
	}
	if !strings.Contains(stderr.String(), "unsupported watchdog action") {
		t.Fatalf("stderr = %q", stderr.String())
	}
}

func TestRunJSONCommandReturnsFailureOnBoardError(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	response := `{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"power","error":{"code":"unknown_power_output","message":"unknown power output"}}`

	code := (App{Client: &fakeClient{response: response}}).Run([]string{"--json", "power", "get", "missing"}, &stdout, &stderr)
	if code != 1 {
		t.Fatalf("Run() exit code = %d stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
	if strings.TrimSpace(stdout.String()) != response {
		t.Fatalf("stdout = %q", stdout.String())
	}
}

func TestRunJSONRejectsOldTextFirmwareOutput(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer

	code := (App{Client: &fakeClient{response: "project=radxa-linkr-debugger"}}).Run([]string{"--json", "status"}, &stdout, &stderr)
	if code != 1 {
		t.Fatalf("Run() exit code = %d stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}

	var got jsonResponse
	if err := json.Unmarshal(stdout.Bytes(), &got); err != nil {
		t.Fatalf("stdout is not JSON: %v stdout=%q", err, stdout.String())
	}
	if got.OK || got.Command != "status" || got.Error == nil || got.Error.Code != "invalid_json" {
		t.Fatalf("json error = %#v", got)
	}
}

func TestRunReportsTransportError(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer

	code := (App{Client: &fakeClient{err: errors.New("dial failed")}}).Run([]string{"status"}, &stdout, &stderr)
	if code != 1 {
		t.Fatalf("Run() exit code = %d", code)
	}
	if !strings.Contains(stderr.String(), "dial failed") {
		t.Fatalf("stderr = %q", stderr.String())
	}
}

func TestRunHelpReturnsSuccess(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer

	code := (App{}).Run([]string{"--help"}, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("Run() exit code = %d stderr=%q", code, stderr.String())
	}
	if !strings.Contains(stderr.String(), "usage: radxa-linkr-debuggerctl") {
		t.Fatalf("stderr = %q", stderr.String())
	}
}

func TestRunVersionReturnsSuccessWithoutBoardAccess(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer

	code := (App{}).Run([]string{"--version"}, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("Run() exit code = %d stderr=%q", code, stderr.String())
	}
	if strings.TrimSpace(stdout.String()) != "radxa-linkr-debuggerctl "+Version {
		t.Fatalf("stdout = %q", stdout.String())
	}
}

func TestRunJSONVersionReturnsSuccessWithoutBoardAccess(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer

	code := (App{}).Run([]string{"--json", "--version"}, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("Run() exit code = %d stderr=%q", code, stderr.String())
	}

	var got map[string]any
	if err := json.Unmarshal(stdout.Bytes(), &got); err != nil {
		t.Fatalf("stdout is not JSON: %v stdout=%q", err, stdout.String())
	}
	if got["schema"] != JSONSchema || got["command"] != "version" || got["version"] != Version {
		t.Fatalf("stdout JSON = %#v", got)
	}
}

func TestDoctorJSONReportsHTTPStatus(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	status := `{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"status","project":"radxa-linkr-debugger"}`

	code := (App{Client: &fakeClient{response: status}}).Run([]string{"--json", "--url", "http://172.29.203.1:8080", "doctor"}, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("Run() exit code = %d stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}

	var got doctorResult
	if err := json.Unmarshal(stdout.Bytes(), &got); err != nil {
		t.Fatalf("stdout is not JSON: %v stdout=%q", err, stdout.String())
	}
	if !got.OK || got.BaseURL != "http://172.29.203.1:8080" || !got.ProbeOK {
		t.Fatalf("doctor JSON = %#v", got)
	}
	if strings.TrimSpace(string(got.Status)) != status {
		t.Fatalf("status = %s", got.Status)
	}
}

func TestDoctorJSONReportsHTTPFailure(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer

	code := (App{Client: &fakeClient{err: errors.New("connection refused")}}).Run([]string{"--json", "doctor"}, &stdout, &stderr)
	if code != 1 {
		t.Fatalf("Run() exit code = %d stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}

	var got doctorResult
	if err := json.Unmarshal(stdout.Bytes(), &got); err != nil {
		t.Fatalf("stdout is not JSON: %v stdout=%q", err, stdout.String())
	}
	if got.OK || got.Error == nil || got.Error.Code != "status_failed" {
		t.Fatalf("doctor JSON = %#v", got)
	}
}

func TestHTTPClientBuildsRequestAndEncodesJSON(t *testing.T) {
	var gotMethod string
	var gotPath string
	var gotQuery string
	var gotBody map[string]string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotMethod = r.Method
		gotPath = r.URL.Path
		gotQuery = r.URL.RawQuery
		if err := json.NewDecoder(r.Body).Decode(&gotBody); err != nil {
			t.Fatalf("decode body: %v", err)
		}
		_, _ = w.Write([]byte(`{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"power"}`))
	}))
	defer server.Close()

	client := HTTPClient{BaseURL: server.URL, Client: server.Client()}
	data, err := client.Do(context.Background(), boardRequest{
		Method: http.MethodPut,
		Path:   "/api/v1/power/12v_out",
		Body:   map[string]string{"state": "on"},
	})
	if err != nil {
		t.Fatalf("Do() error = %v", err)
	}
	if gotMethod != http.MethodPut || gotPath != "/api/v1/power/12v_out" || gotQuery != "" || gotBody["state"] != "on" {
		t.Fatalf("method=%q path=%q query=%q body=%#v", gotMethod, gotPath, gotQuery, gotBody)
	}
	if !strings.Contains(string(data), `"ok":true`) {
		t.Fatalf("data = %s", data)
	}
}

func TestNewWSClientURLNormalizesBaseAndWSURL(t *testing.T) {
	client := NewWSClientURL("172.29.203.1:8080", " ws://172.29.203.1:8080/api/v1/ws/2 ")
	if client.baseURL != DefaultBaseURL {
		t.Fatalf("baseURL = %q", client.baseURL)
	}
	if client.wsURL != "ws://172.29.203.1:8080/api/v1/ws/2" {
		t.Fatalf("wsURL = %q", client.wsURL)
	}
}

func TestWSClientConnectAndSend(t *testing.T) {
	upgrader := websocket.Upgrader{}
	var got map[string]any
	done := make(chan struct{}, 1)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			t.Fatalf("upgrade: %v", err)
		}
		defer conn.Close()
		if err := conn.ReadJSON(&got); err != nil {
			t.Fatalf("read json: %v", err)
		}
		done <- struct{}{}
		if err := conn.WriteJSON(wsStatusSnapshot{
			Type:         "snapshot",
			Topic:        "status",
			Schema:       JSONSchema,
			Sequence:     1,
			PowerOutputs: []tuiStatusPowerOutput{{Name: "5v_out", State: "on", Value: 1}},
			Switches:     tuiStatusSwitches{SD: tuiStatusSwitchRoute{Route: "target"}},
			BoardMonitoring: boardMonitoring{
				Temperature: monitoringTemperature{
					monitoringAvailability: monitoringAvailability{Available: false, Reason: "no_zephyr_temperature_device"},
				},
				Heap: monitoringHeap{
					monitoringAvailability: monitoringAvailability{Available: true},
					Source:                 "system_heap",
					FreeBytes:              6144,
					AllocatedBytes:         2048,
					MaxAllocatedBytes:      3072,
					TotalBytes:             8192,
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
		}); err != nil {
			t.Fatalf("write json: %v", err)
		}
	}))
	defer server.Close()

	client := newTestWSClient(t, server.URL)
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	if err := client.Connect(ctx); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}
	defer client.Close()
	if err := client.Send(ctx, wsCommandRequest{Type: "subscribe", Topic: "live", RateHz: tuiADCSubscribeRateHz}); err != nil {
		t.Fatalf("Send() error = %v", err)
	}
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for websocket request")
	}
	if got["type"] != "subscribe" || got["topic"] != "live" || got["rate_hz"] != float64(tuiADCSubscribeRateHz) {
		t.Fatalf("got request = %#v", got)
	}
}

func TestWSClientCloseAdvancesGeneration(t *testing.T) {
	client := newTestWSClient(t, DefaultBaseURL)
	before := client.Generation()
	if before == 0 {
		t.Fatalf("expected non-zero generation")
	}
	if err := client.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}
	after := client.Generation()
	if after <= before {
		t.Fatalf("generation did not advance: before=%d after=%d", before, after)
	}
	if client.IsCurrentGeneration(before) {
		t.Fatalf("old generation %d should be stale after close", before)
	}
	if !client.IsCurrentGeneration(after) {
		t.Fatalf("new generation %d should be current", after)
	}
}

func TestNextStreamMessageParsesResult(t *testing.T) {
	upgrader := websocket.Upgrader{}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			t.Fatalf("upgrade: %v", err)
		}
		defer conn.Close()
		if err := conn.WriteJSON(map[string]any{
			"type":    "result",
			"schema":  JSONSchema,
			"command": "power_set",
			"ok":      true,
			"status":  "ok",
		}); err != nil {
			t.Fatalf("write json: %v", err)
		}
	}))
	defer server.Close()

	wsClient := newTestWSClient(t, server.URL)
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	if err := wsClient.Connect(ctx); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}
	defer wsClient.Close()

	msg := nextStreamMessage(wsClient, 2*time.Second)().(tuiStreamMsg)
	if msg.err != nil {
		t.Fatalf("nextStreamMessage error = %v", msg.err)
	}
	if msg.result == nil || msg.result.Command != "power_set" || msg.result.Status != "ok" {
		t.Fatalf("result = %#v", msg.result)
	}
}

func TestMultipleWSClientsConnectIndependently(t *testing.T) {
	upgrader := websocket.Upgrader{}
	connected := make(chan struct{}, 2)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			t.Fatalf("upgrade: %v", err)
		}
		defer conn.Close()
		connected <- struct{}{}
		if err := conn.WriteJSON(map[string]any{
			"type":    "result",
			"schema":  JSONSchema,
			"command": "subscribe",
			"ok":      true,
			"status":  "ok",
		}); err != nil {
			t.Fatalf("write json: %v", err)
		}
		select {}
	}))
	defer server.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	clients := []*WSClient{newTestWSClient(t, server.URL), newTestWSClient(t, server.URL)}
	for _, client := range clients {
		if err := client.Connect(ctx); err != nil {
			t.Fatalf("Connect() error = %v", err)
		}
		defer client.Close()
	}

	for i := 0; i < len(clients); i++ {
		select {
		case <-connected:
		case <-time.After(2 * time.Second):
			t.Fatal("timed out waiting for websocket connections")
		}
	}

	for _, client := range clients {
		msg := nextStreamMessage(client, 2*time.Second)().(tuiStreamMsg)
		if msg.err != nil {
			t.Fatalf("nextStreamMessage error = %v", msg.err)
		}
		if msg.result == nil || msg.result.Command != "subscribe" || msg.result.Status != "ok" {
			t.Fatalf("result = %#v", msg.result)
		}
	}
}

func TestIsDebugBoardStatus(t *testing.T) {
	output := `{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"status","project":"radxa-linkr-debugger"}`
	if !IsDebugBoardStatus(output) {
		t.Fatal("expected debugboard status output to be recognized")
	}
	if IsDebugBoardStatus("project=other") {
		t.Fatal("unexpected match for unrelated status output")
	}
}
