package hostcli

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

const tuiPollInterval = time.Second / 60
const tuiADCSubscribeRateHz = 60
const tuiHistoryLimit = 240

type tuiStatusPowerOutput struct {
	Name  string `json:"name"`
	State string `json:"state"`
	Value int    `json:"value"`
}

type tuiStatusSD struct {
	Route string `json:"route"`
}

type tuiStatusWatchdog struct {
	Automatic           bool   `json:"automatic"`
	Healthy             bool   `json:"healthy"`
	Supported           bool   `json:"supported"`
	Armed               bool   `json:"armed"`
	TimeoutMS           uint32 `json:"timeout_ms"`
	BootloaderOnTimeout bool   `json:"bootloader_on_timeout"`
	FailingService      string `json:"failing_service"`
}

type tuiActionMsg struct {
	status string
	err    error
}

type tuiStreamMsg struct {
	snapshot  *wsStatusSnapshot
	telemetry *adcResponse
	result    *wsEnvelope
	err       error
	generation uint64
}

type tuiModel struct {
	app          App
	baseURL      string
	wsURL        string
	timeout      time.Duration
	width        int
	height       int
	paused       bool
	err          error
	status       string
	history      map[string][]int32
	latest       map[string]adcReading
	controlIdx   int
	scrollOffset int
	powerStates  map[string]bool
	sdRoute      string
	monitoring   boardMonitoring
	connectPending bool
	streamPending  bool
	closed       bool
	channelIDs   []string
	wsClient     *WSClient
}

type liveSessionCreateResponse struct {
	Schema    string `json:"schema"`
	OK        bool   `json:"ok"`
	Command   string `json:"command"`
	Action    string `json:"action"`
	SessionID uint32 `json:"session_id"`
	WSURL     string `json:"ws_url"`
}

func runTUI(app App, baseURL string, timeout time.Duration, stdout io.Writer, stderr io.Writer) int {
	wsURL, err := requestLiveSessionWSURL(app.withBaseURL(resolveBaseURL(baseURL)), timeout)
	if err != nil {
		fmt.Fprintln(stderr, err)
		return 1
	}
	model := tuiModel{
		app:         app,
		baseURL:     resolveBaseURL(baseURL),
		wsURL:       wsURL,
		timeout:     timeout,
		status:      "Connecting…",
		history:     make(map[string][]int32),
		latest:      make(map[string]adcReading),
		powerStates: make(map[string]bool),
		sdRoute:     "target",
		channelIDs:  []string{"5v_out", "12v_out", "20v_out"},
		wsClient:    NewWSClientURL(resolveBaseURL(baseURL), wsURL),
	}

	program := tea.NewProgram(model, tea.WithOutput(stdout))
	finalModel, err := program.Run()
	if err != nil {
		fmt.Fprintln(stderr, err)
		return 1
	}

	if m, ok := finalModel.(tuiModel); ok && m.err != nil {
		fmt.Fprintln(stderr, m.err)
		return 1
	}

	return 0
}

func requestLiveSessionWSURL(app App, timeout time.Duration) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	data, err := app.Client.Do(ctx, boardRequest{Method: http.MethodPost, Path: "/api/v1/live-sessions"})
	if err != nil {
		return "", err
	}
	var response liveSessionCreateResponse
	if err := json.Unmarshal(data, &response); err != nil {
		return "", fmt.Errorf("decode live session response: %w", err)
	}
	if !response.OK || strings.TrimSpace(response.WSURL) == "" {
		return "", fmt.Errorf("missing websocket URL in live session response")
	}
	return response.WSURL, nil
}

func (m tuiModel) Init() tea.Cmd {
	return tea.Batch(connectStream(m.wsClient, m.timeout), nextPollTick())
}

func (m tuiModel) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.width = msg.Width
		m.height = msg.Height
		return m, nil
	case tea.KeyMsg:
		switch msg.String() {
		case "ctrl+c", "q":
			m.closed = true
			m.connectPending = false
			m.streamPending = false
			_ = m.wsClient.Close()
			return m, tea.Quit
		case "p":
			m.paused = !m.paused
			if m.paused {
				m.status = "Paused"
				return m, nil
			}
			m.status = "Resumed"
			if !m.wsClient.IsConnected() && !m.connectPending {
				m.connectPending = true
				return m, tea.Batch(connectStream(m.wsClient, m.timeout), nextPollTick())
			}
			return m, nextPollTick()
		case "r":
			m.status = "Refreshing…"
			if !m.wsClient.IsConnected() && !m.connectPending {
				m.connectPending = true
				return m, connectStream(m.wsClient, m.timeout)
			}
			return m, nil
		case "up", "k":
			m.controlIdx = moveControlSelection(m, -1, 0)
			return m, nil
		case "down", "j":
			m.controlIdx = moveControlSelection(m, 1, 0)
			return m, nil
		case "left", "h":
			m.controlIdx = moveControlSelection(m, 0, -1)
			return m, nil
		case "right", "l":
			m.controlIdx = moveControlSelection(m, 0, 1)
			return m, nil
		case "enter", " ":
			target := controlTargets[m.controlIdx]
			return m, performControlAction(m.wsClient, m.timeout, target, m.powerStates[target])
		case "t":
			return m, setSDRoute(m.wsClient, m.timeout, "target")
		case "u":
			return m, setSDRoute(m.wsClient, m.timeout, "usb-reader")
		case "pgdown", "ctrl+d", "]":
			m.scrollOffset += 3
			return m, nil
		case "pgup", "ctrl+u", "[":
			m.scrollOffset -= 3
			if m.scrollOffset < 0 {
				m.scrollOffset = 0
			}
			return m, nil
		}
	case tuiActionMsg:
		if m.closed {
			return m, nil
		}
		m.err = msg.err
		if msg.err != nil {
			m.status = msg.err.Error()
			return m, nil
		}
		m.status = msg.status
		if m.wsClient.IsConnected() && !m.streamPending {
			m.streamPending = true
			return m, nextStreamMessage(m.wsClient, m.timeout)
		}
		return m, nil
	case tuiStreamMsg:
		if !m.wsClient.IsCurrentGeneration(msg.generation) || m.closed {
			return m, nil
		}
		m.connectPending = false
		m.streamPending = false
		m.err = msg.err
		if msg.err != nil {
			m.status = "Stream error"
			_ = m.wsClient.Close()
			return m, nextPollTick()
		}
		m.status = "Live"
		if msg.result != nil {
			if msg.result.Command != "" {
				m.status = fmt.Sprintf("ws %s %s", msg.result.Command, msg.result.Status)
			}
		}
		if msg.snapshot != nil {
			for _, output := range msg.snapshot.PowerOutputs {
				m.powerStates[output.Name] = output.Value != 0 || output.State == "on"
			}
			if msg.snapshot.SD.Route != "" {
				m.sdRoute = msg.snapshot.SD.Route
			}
			m.monitoring = msg.snapshot.BoardMonitoring
		}
		if msg.telemetry != nil {
			for _, reading := range msg.telemetry.Readings {
				m.latest[reading.Name] = reading
				if reading.PowerEnabled != nil {
					m.powerStates[reading.Name] = *reading.PowerEnabled
				}
				ma := currentMilliampEstimate(reading)
				series := append(m.history[reading.Name], ma)
				if len(series) > tuiHistoryLimit {
					series = series[len(series)-tuiHistoryLimit:]
				}
				m.history[reading.Name] = series
			}
		}
		if m.wsClient.IsConnected() && !m.streamPending {
			m.streamPending = true
			return m, nextStreamMessage(m.wsClient, m.timeout)
		}
		return m, nil
	case time.Time:
		if m.closed {
			return m, nil
		}
		if m.paused {
			return m, nil
		}
		if !m.wsClient.IsConnected() {
			if !m.connectPending {
				m.connectPending = true
				return m, tea.Batch(connectStream(m.wsClient, m.timeout), nextPollTick())
			}
			return m, nextPollTick()
		}
		if !m.streamPending {
			m.streamPending = true
			return m, tea.Batch(nextStreamMessage(m.wsClient, m.timeout), nextPollTick())
		}
		return m, nextPollTick()
	}

	return m, nil
}

var controlTargets = []string{"12v_out", "5v_out", "5v_ws", "20v_out"}

func (m tuiModel) View() string {
	titleStyle := lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("205"))
	mutedStyle := lipgloss.NewStyle().Foreground(lipgloss.Color("241"))
	header := titleStyle.Render("Agent DebugBoard TUI")
	header += "\n" + mutedStyle.Render(fmt.Sprintf("url=%s  status=%s  keys: q quit · p pause · r refresh · ↑/↓ select power · Enter toggle · t/u sd route · [/]/PgUp/PgDn scroll", m.baseURL, m.status))

	chartHeight := 4
	columnGap := 2

	lines := []string{header, ""}
	chartRow := buildChartRow(m, chartHeight, columnGap)
	lines = append(lines, chartRow)

	bodyLines := []string{""}
	bodyLines = append(bodyLines, renderControlTab(m)...)

	if m.err != nil {
		bodyLines = append(bodyLines, "", lipgloss.NewStyle().Foreground(lipgloss.Color("196")).Render("error: "+m.err.Error()))
	}

	bodyLines = append(bodyLines, "", mutedStyle.Render("No-args starts the TUI. Existing command mode is unchanged when args are provided."))

	if m.height > 0 {
		fixedHeight := len(strings.Split(header, "\n")) + len(strings.Split(chartRow, "\n"))
		availableBodyHeight := m.height - fixedHeight
		if availableBodyHeight > 0 && len(bodyLines) > availableBodyHeight {
			maxOffset := len(bodyLines) - availableBodyHeight
			offset := m.scrollOffset
			if offset > maxOffset {
				offset = maxOffset
			}
			if offset < 0 {
				offset = 0
			}
			end := offset + availableBodyHeight
			if end > len(bodyLines) {
				end = len(bodyLines)
			}
			bodyLines = bodyLines[offset:end]
			bodyLines = append(bodyLines, mutedStyle.Render(fmt.Sprintf("scroll %d/%d", offset, maxOffset)))
		}
	}

	lines = append(lines, bodyLines...)
	return strings.Join(lines, "\n")
}

func buildChartRow(m tuiModel, chartHeight int, columnGap int) string {
	minContentWidth := 10
	chartWidth := 18
	if m.width > 0 {
		chartWidth = maxInt(minContentWidth, (m.width-2*columnGap)/3)
	}

	for {
		chartBlocks := make([]string, 0, len(m.channelIDs))
		for _, channel := range m.channelIDs {
			reading := m.latest[channel]
			ma := currentMilliampEstimate(reading)
			currentText := fmt.Sprintf("%5.2fA", float64(ma)/1000.0)
			detailText := fmt.Sprintf("%4dmA", ma)
			if reading.CurrentUA != nil {
				currentText = currentTextFromMicroamps(*reading.CurrentUA)
			}
			power := "?"
			if reading.PowerEnabled != nil {
				if *reading.PowerEnabled {
					power = "on"
				} else {
					power = "off"
				}
			}
			chartBlocks = append(chartBlocks, renderBarChart(channel, currentText, detailText, power, m.history[channel], ma, chartWidth, chartHeight, chartMaxMilliamp(channel)))
		}

		chartRow := joinBlocksHorizontally(chartBlocks, columnGap)
		if m.width <= 0 || lipgloss.Width(chartRow) <= m.width || chartWidth <= minContentWidth {
			return chartRow
		}

		chartWidth--
	}
}

func maxInt(a int, b int) int {
	if a > b {
		return a
	}
	return b
}

func chartMaxMilliamp(channel string) int32 {
	return 5000
}

func renderControlTab(m tuiModel) []string {
	lines := []string{"Controls:", "  ↑/↓ select   Enter/Space toggle power   t target route   u usb-reader route", ""}
	contentWidth := m.width
	if contentWidth <= 0 {
		contentWidth = 80
	}
	rows, chips := buildControlRows(m, contentWidth)
	highlight := lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("229")).Background(lipgloss.Color("62"))
	for _, row := range rows {
		parts := make([]string, 0, len(row))
		for _, idx := range row {
			part := chips[idx]
			if idx == m.controlIdx {
				part = highlight.Render(part)
			}
			parts = append(parts, part)
		}
		lines = append(lines, strings.Join(parts, "  "))
	}
	lines = append(lines, "", fmt.Sprintf("  sd route = %s", m.sdRoute))
	lines = append(lines, "  "+formatMonitoringSummary(m.monitoring))
	return lines
}

func buildControlRows(m tuiModel, contentWidth int) ([][]int, []string) {
	chips := make([]string, 0, len(controlTargets))
	for _, output := range controlTargets {
		state := "off"
		if m.powerStates[output] {
			state = "on"
		}
		chips = append(chips, fmt.Sprintf("%s:%s", output, state))
	}

	rows := make([][]int, 0, 2)
	currentRow := make([]int, 0, len(chips))
	currentWidth := 0
	for idx, chip := range chips {
		chipWidth := lipgloss.Width(chip)
		candidateWidth := currentWidth
		if len(currentRow) > 0 {
			candidateWidth += 2
		}
		candidateWidth += chipWidth
		if candidateWidth > contentWidth && len(currentRow) > 0 {
			rows = append(rows, currentRow)
			currentRow = []int{idx}
			currentWidth = chipWidth
			continue
		}
		currentRow = append(currentRow, idx)
		currentWidth = candidateWidth
	}
	if len(currentRow) > 0 {
		rows = append(rows, currentRow)
	}

	return rows, chips
}

func moveControlSelection(m tuiModel, rowDelta int, colDelta int) int {
	contentWidth := m.width
	if contentWidth <= 0 {
		contentWidth = 80
	}
	rows, _ := buildControlRows(m, contentWidth)
	if len(rows) == 0 {
		return m.controlIdx
	}

	rowIdx := 0
	colIdx := 0
	found := false
	for r, row := range rows {
		for c, idx := range row {
			if idx == m.controlIdx {
				rowIdx = r
				colIdx = c
				found = true
				break
			}
		}
		if found {
			break
		}
	}

	if rowDelta != 0 {
		nextRow := rowIdx + rowDelta
		if nextRow < 0 {
			nextRow = 0
		}
		if nextRow >= len(rows) {
			nextRow = len(rows) - 1
		}
		nextCol := colIdx
		if nextCol >= len(rows[nextRow]) {
			nextCol = len(rows[nextRow]) - 1
		}
		return rows[nextRow][nextCol]
	}

	if colDelta != 0 {
		nextCol := colIdx + colDelta
		if nextCol >= 0 && nextCol < len(rows[rowIdx]) {
			return rows[rowIdx][nextCol]
		}
		if colDelta < 0 && rowIdx > 0 {
			prevRow := rows[rowIdx-1]
			return prevRow[len(prevRow)-1]
		}
		if colDelta > 0 && rowIdx+1 < len(rows) {
			return rows[rowIdx+1][0]
		}
	}

	return m.controlIdx
}

func nextPollTick() tea.Cmd {
	return tea.Tick(tuiPollInterval, func(t time.Time) tea.Msg { return t })
}

func connectStream(wsClient *WSClient, timeout time.Duration) tea.Cmd {
	generation := wsClient.Generation()
	return func() tea.Msg {
		ctx, cancel := context.WithTimeout(context.Background(), timeout)
		defer cancel()
		if !wsClient.IsCurrentGeneration(generation) {
			return tuiStreamMsg{generation: generation}
		}
		if err := wsClient.Connect(ctx); err != nil {
			return tuiStreamMsg{err: err, generation: generation}
		}
		if !wsClient.IsCurrentGeneration(generation) {
			_ = wsClient.Close()
			return tuiStreamMsg{generation: generation}
		}
		if err := wsClient.Send(ctx, wsCommandRequest{Type: "subscribe", Topic: "live", RateHz: tuiADCSubscribeRateHz}); err != nil {
			return tuiStreamMsg{err: err, generation: generation}
		}
		return tuiStreamMsg{result: &wsEnvelope{Type: "result", Command: "subscribe", Status: "ok"}, generation: generation}
	}
}

func nextStreamMessage(wsClient *WSClient, timeout time.Duration) tea.Cmd {
	generation := wsClient.Generation()
	return func() tea.Msg {
		ctx, cancel := context.WithTimeout(context.Background(), timeout)
		defer cancel()
		if !wsClient.IsCurrentGeneration(generation) {
			return tuiStreamMsg{generation: generation}
		}

		var raw map[string]any
		if err := wsClient.Recv(ctx, &raw); err != nil {
			return tuiStreamMsg{err: err, generation: generation}
		}

		encoded, err := json.Marshal(raw)
		if err != nil {
			return tuiStreamMsg{err: err, generation: generation}
		}

		var base wsEnvelope
		if err := json.Unmarshal(encoded, &base); err != nil {
			return tuiStreamMsg{err: err, generation: generation}
		}

		switch base.Type {
		case "snapshot":
			var snapshot wsStatusSnapshot
			if err := json.Unmarshal(encoded, &snapshot); err != nil {
				return tuiStreamMsg{err: err, generation: generation}
			}
			return tuiStreamMsg{snapshot: &snapshot, generation: generation}
		case "telemetry":
			response, err := transformADCResponse(string(encoded))
			if err != nil {
				return tuiStreamMsg{err: err, generation: generation}
			}
			return tuiStreamMsg{telemetry: response, generation: generation}
		case "result":
			return tuiStreamMsg{result: &base, generation: generation}
		case "error":
			if base.Error != nil {
				return tuiStreamMsg{err: fmt.Errorf("%s: %s", base.Error.Code, base.Error.Message), generation: generation}
			}
			return tuiStreamMsg{err: fmt.Errorf("websocket error"), generation: generation}
		default:
			return tuiStreamMsg{err: fmt.Errorf("unknown websocket message type %q", base.Type), generation: generation}
		}
	}
}

func performControlAction(wsClient *WSClient, timeout time.Duration, output string, currentState bool) tea.Cmd {
	nextState := "on"
	if currentState {
		nextState = "off"
	}
	return func() tea.Msg {
		if wsClient == nil || !wsClient.IsConnected() {
			return tuiActionMsg{err: fmt.Errorf("websocket not connected")}
		}
		ctx, cancel := context.WithTimeout(context.Background(), timeout)
		defer cancel()
		err := wsClient.Send(ctx, wsCommandRequest{Type: "command", Command: "power_set", Output: output, State: nextState})
		if err != nil {
			return tuiActionMsg{err: err}
		}
		return tuiActionMsg{status: fmt.Sprintf("power %s=%s", output, nextState)}
	}
}

func setSDRoute(wsClient *WSClient, timeout time.Duration, route string) tea.Cmd {
	return func() tea.Msg {
		if wsClient == nil || !wsClient.IsConnected() {
			return tuiActionMsg{err: fmt.Errorf("websocket not connected")}
		}
		ctx, cancel := context.WithTimeout(context.Background(), timeout)
		defer cancel()
		err := wsClient.Send(ctx, wsCommandRequest{Type: "command", Command: "sd_route", Route: route})
		if err != nil {
			return tuiActionMsg{err: err}
		}
		return tuiActionMsg{status: fmt.Sprintf("sd route=%s", route)}
	}
}

func requestString(app App, request boardRequest, timeout time.Duration) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	data, err := app.Client.Do(ctx, request)
	if err != nil {
		return "", err
	}
	return string(data), nil
}

func currentMilliampEstimate(reading adcReading) int32 {
	if reading.MAEst != nil {
		return *reading.MAEst
	}
	if reading.CurrentUA != nil {
		return *reading.CurrentUA / 1000
	}
	if reading.SensorValue != nil {
		return sensorValueToMicroamps(*reading.SensorValue) / 1000
	}
	return 0
}
