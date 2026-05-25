package hostcli

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

type wsEnvelope struct {
	Type     string          `json:"type"`
	Topic    string          `json:"topic,omitempty"`
	Schema   string          `json:"schema,omitempty"`
	Sequence uint32          `json:"sequence,omitempty"`
	ID       string          `json:"id,omitempty"`
	Command  string          `json:"command,omitempty"`
	OK       *bool           `json:"ok,omitempty"`
	Status   string          `json:"status,omitempty"`
	Error    *JSONError      `json:"error,omitempty"`
	Raw      json.RawMessage `json:"-"`
}

type wsStatusSnapshot struct {
	Type            string                 `json:"type"`
	Topic           string                 `json:"topic"`
	Schema          string                 `json:"schema"`
	Sequence        uint32                 `json:"sequence"`
	PowerOutputs    []tuiStatusPowerOutput `json:"power_outputs"`
	Switches        tuiStatusSwitches      `json:"switches"`
	Watchdog        tuiStatusWatchdog      `json:"watchdog"`
	BoardMonitoring boardMonitoring        `json:"board_monitoring"`
}

type tuiStatusSwitchRoute struct {
	Route string `json:"route"`
}

type tuiStatusSwitches struct {
	SD  tuiStatusSwitchRoute `json:"sd"`
	USB tuiStatusSwitchRoute `json:"usb"`
}

type wsADCMessage struct {
	Type     string       `json:"type"`
	Topic    string       `json:"topic"`
	Schema   string       `json:"schema"`
	Sequence uint32       `json:"sequence"`
	Readings []adcReading `json:"readings"`
}

type wsCommandRequest struct {
	Type      string `json:"type"`
	Command   string `json:"command,omitempty"`
	Topic     string `json:"topic,omitempty"`
	Output    string `json:"output,omitempty"`
	Name      string `json:"name,omitempty"`
	State     string `json:"state,omitempty"`
	Route     string `json:"route,omitempty"`
	GPIO      string `json:"gpio,omitempty"`
	Direction string `json:"direction,omitempty"`
	Value     int    `json:"value,omitempty"`
	RateHz    int    `json:"rate_hz,omitempty"`
}

type WSClient struct {
	baseURL string
	wsURL   string
	conn    *websocket.Conn
	mu      sync.Mutex
	readMu  sync.Mutex
	writeMu sync.Mutex
	gen     uint64
}

func NewWSClientURL(baseURL string, wsURL string) *WSClient {
	return &WSClient{baseURL: resolveBaseURL(baseURL), wsURL: strings.TrimSpace(wsURL), gen: 1}
}

func (c *WSClient) Connect(ctx context.Context) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.conn != nil {
		return nil
	}

	wsURL := c.wsURL
	if strings.TrimSpace(wsURL) == "" {
		return fmt.Errorf("missing dedicated websocket URL")
	}

	dialer := websocket.Dialer{}
	conn, resp, err := dialer.DialContext(ctx, wsURL, http.Header{})
	if err != nil {
		if resp != nil {
			return fmt.Errorf("dial websocket %s: HTTP %s", wsURL, resp.Status)
		}
		return fmt.Errorf("dial websocket %s: %w", wsURL, err)
	}

	c.conn = conn
	return nil
}

func (c *WSClient) Close() error {
	c.mu.Lock()
	conn := c.conn
	c.conn = nil
	c.gen++
	c.mu.Unlock()

	if conn == nil {
		return nil
	}

	c.writeMu.Lock()
	_ = conn.WriteControl(websocket.CloseMessage,
		websocket.FormatCloseMessage(websocket.CloseNormalClosure, "client closing"),
		time.Now().Add(200*time.Millisecond))
	c.writeMu.Unlock()

	return conn.Close()
}

func (c *WSClient) IsConnected() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.conn != nil
}

func (c *WSClient) Generation() uint64 {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.gen
}

func (c *WSClient) IsCurrentGeneration(generation uint64) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.gen == generation
}

func (c *WSClient) Send(ctx context.Context, msg any) error {
	c.mu.Lock()
	conn := c.conn
	c.mu.Unlock()

	if conn == nil {
		return fmt.Errorf("websocket not connected")
	}

	c.writeMu.Lock()
	defer c.writeMu.Unlock()

	deadline, ok := ctx.Deadline()
	if ok {
		_ = conn.SetWriteDeadline(deadline)
	} else {
		_ = conn.SetWriteDeadline(time.Time{})
	}

	return conn.WriteJSON(msg)
}

func (c *WSClient) Recv(ctx context.Context, dst any) error {
	c.mu.Lock()
	conn := c.conn
	c.mu.Unlock()

	if conn == nil {
		return fmt.Errorf("websocket not connected")
	}

	c.readMu.Lock()
	defer c.readMu.Unlock()

	deadline, ok := ctx.Deadline()
	if ok {
		_ = conn.SetReadDeadline(deadline)
	} else {
		_ = conn.SetReadDeadline(time.Time{})
	}

	return conn.ReadJSON(dst)
}
