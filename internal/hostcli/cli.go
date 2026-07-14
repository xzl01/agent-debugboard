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
	"flag"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"regexp"
	"strconv"
	"strings"
	"time"
)

const (
	PromptText          = "linkr-debugger:~$"
	DefaultBaseURL      = "http://172.29.203.1:8080"
	projectStatusLine   = "project=radxa-linkr-debugger"
	JSONSchema          = "radxa-linkr-debugger.v1"
	internalPowerOutput = "5v_ws"
)

var ansiRE = regexp.MustCompile(`\x1b\[[0-9;]*m`)

var Version = "dev"

type App struct {
	Client BoardClient
	RunTUI func(baseURL string, timeout time.Duration, stdout io.Writer, stderr io.Writer) int
}

type BoardClient interface {
	Do(ctx context.Context, request boardRequest) ([]byte, error)
}

type boardRequest struct {
	Method string
	Path   string
	Query  url.Values
	Body   any
}

type durationFlag struct {
	value time.Duration
}

type JSONError struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

type jsonEnvelope struct {
	Schema  string     `json:"schema"`
	OK      *bool      `json:"ok"`
	Command string     `json:"command"`
	Error   *JSONError `json:"error,omitempty"`
}

type jsonResponse struct {
	Schema  string     `json:"schema"`
	OK      bool       `json:"ok"`
	Command string     `json:"command"`
	Error   *JSONError `json:"error,omitempty"`
}

type doctorResult struct {
	Schema     string          `json:"schema"`
	OK         bool            `json:"ok"`
	Command    string          `json:"command"`
	CLIVersion string          `json:"cli_version"`
	BaseURL    string          `json:"base_url"`
	ProbeOK    bool            `json:"probe_ok"`
	Status     json.RawMessage `json:"status,omitempty"`
	StatusText string          `json:"status_text,omitempty"`
	Error      *JSONError      `json:"error,omitempty"`
}

type jsonValidationError struct {
	code    string
	message string
}

func (e jsonValidationError) Error() string {
	return e.message
}

func NewApp() App {
	return App{Client: HTTPClient{BaseURL: DefaultBaseURL, Client: http.DefaultClient}}
}

func (d *durationFlag) String() string {
	return d.value.String()
}

func (d *durationFlag) Set(value string) error {
	if parsed, err := time.ParseDuration(value); err == nil {
		d.value = parsed
		return nil
	}

	seconds, err := strconv.ParseFloat(value, 64)
	if err != nil {
		return fmt.Errorf("timeout must be a Go duration like 2s or seconds like 0.5")
	}
	if seconds <= 0 {
		return errors.New("timeout must be greater than zero")
	}

	d.value = time.Duration(seconds * float64(time.Second))
	return nil
}

func (a App) Run(args []string, stdout io.Writer, stderr io.Writer) int {
	timeout := &durationFlag{value: 2 * time.Second}
	var baseURL string
	var raw bool
	var showVersion bool
	var jsonOutput bool
	var verbose bool

	fs := flag.NewFlagSet("radxa-linkr-debuggerctl", flag.ContinueOnError)
	fs.SetOutput(stderr)
	fs.StringVar(&baseURL, "url", "", "Radxa Linkr Debugger HTTP base URL, for example http://172.29.203.1:8080")
	fs.StringVar(&baseURL, "addr", "", "Radxa Linkr Debugger HTTP address or base URL, for example 172.29.203.1:8080")
	fs.StringVar(&baseURL, "port", "", "deprecated alias for --url")
	fs.Var(timeout, "timeout", "command timeout, for example 2s or 0.5")
	fs.BoolVar(&raw, "raw", false, "send args as a raw shell command")
	fs.BoolVar(&jsonOutput, "json", false, "request and validate JSON output")
	fs.BoolVar(&verbose, "v", false, "request verbose human output")
	fs.BoolVar(&verbose, "verbose", false, "request verbose human output")
	fs.BoolVar(&showVersion, "version", false, "print version and exit")
	fs.Usage = func() {
		fmt.Fprintf(stderr, "usage: radxa-linkr-debuggerctl [--url URL] [--timeout 2s] [--json] [-v] [--version] <command> [args...]\n\n")
		fmt.Fprintf(stderr, "examples:\n")
		fmt.Fprintf(stderr, "  radxa-linkr-debuggerctl status\n")
		fmt.Fprintf(stderr, "  radxa-linkr-debuggerctl --json status\n")
		fmt.Fprintf(stderr, "  radxa-linkr-debuggerctl doctor\n")
		fmt.Fprintf(stderr, "  radxa-linkr-debuggerctl power set 12v_out on\n")
		fmt.Fprintf(stderr, "  radxa-linkr-debuggerctl adc read\n")
		fmt.Fprintf(stderr, "  radxa-linkr-debuggerctl adc read -v 5v_out\n")
		fmt.Fprintf(stderr, "  radxa-linkr-debuggerctl watchdog status\n\n")
		fs.PrintDefaults()
	}

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}

	if showVersion {
		if jsonOutput {
			writeJSON(stdout, map[string]any{
				"schema":  JSONSchema,
				"ok":      true,
				"command": "version",
				"version": Version,
			})
			return 0
		}
		fmt.Fprintf(stdout, "radxa-linkr-debuggerctl %s\n", Version)
		return 0
	}

	commandArgs := fs.Args()
	if len(commandArgs) == 0 {
		if !raw && !jsonOutput {
			return a.runTUI(resolveBaseURL(baseURL), timeout.value, stdout, stderr)
		}
		if jsonOutput {
			writeJSONError(stdout, "radxa-linkr-debuggerctl", "missing_command",
				"missing command, for example: adc read")
			return 2
		}
		fmt.Fprintln(stderr, "missing command, for example: adc read")
		fs.Usage()
		return 2
	}

	if !raw && commandArgs[0] == "doctor" {
		if len(commandArgs) != 1 {
			if jsonOutput {
				writeJSONError(stdout, "doctor", "usage", "usage: radxa-linkr-debuggerctl doctor")
			} else {
				fmt.Fprintln(stderr, "usage: radxa-linkr-debuggerctl doctor")
			}
			return 2
		}
		return a.runDoctor(resolveBaseURL(baseURL), timeout.value, jsonOutput, stdout, stderr)
	}
	if raw {
		if jsonOutput {
			writeJSONError(stdout, commandName(commandArgs, raw), "unsupported_raw", "raw shell commands are not available over HTTP")
		} else {
			fmt.Fprintln(stderr, "raw shell commands are not available over HTTP")
		}
		return 2
	}

	adcReadCommand := isADCReadCommand(commandArgs)
	adcVerbose := adcReadCommand && (verbose || hasArg(commandArgs, "-v") || hasArg(commandArgs, "--verbose"))
	wireArgs := append([]string(nil), commandArgs...)
	if verbose && !jsonOutput && !adcReadCommand && !hasArg(wireArgs, "-v") && !hasArg(wireArgs, "--verbose") {
		wireArgs = append(wireArgs, "-v")
	}
	if (jsonOutput || adcReadCommand) && !hasArg(wireArgs, "--json") {
		wireArgs = append(wireArgs, "--json")
	}

	request, _, err := requestFromArgs(wireArgs)
	if err != nil {
		if jsonOutput {
			writeJSONError(stdout, commandName(commandArgs, raw), "usage", err.Error())
		} else {
			fmt.Fprintln(stderr, err)
		}
		return 2
	}

	output, err := a.request(resolveBaseURL(baseURL), request, timeout.value)
	if err != nil {
		if jsonOutput {
			writeJSONError(stdout, commandName(commandArgs, raw), "transport_error", err.Error())
		} else {
			fmt.Fprintln(stderr, err)
		}
		return 1
	}

	cleaned := strings.TrimSpace(output)
	filtered, err := filterInternalPowerOutput(cleaned)
	if err != nil {
		if jsonOutput {
			writeJSONError(stdout, commandName(commandArgs, raw), "invalid_json", err.Error())
		} else {
			fmt.Fprintln(stderr, err)
		}
		return 1
	}
	cleaned = filtered
	if adcReadCommand {
		if jsonOutput {
			return writeADCReadJSON(stdout, cleaned, commandName(commandArgs, raw))
		}
		return writeADCReadText(stdout, stderr, cleaned, adcVerbose)
	}

	if jsonOutput || looksLikeJSON(cleaned) {
		return writeValidatedJSON(stdout, cleaned, commandName(commandArgs, raw), !raw)
	}

	if cleaned != "" {
		fmt.Fprintln(stdout, cleaned)
	}
	return 0
}

func (a App) runTUI(baseURL string, timeout time.Duration, stdout io.Writer, stderr io.Writer) int {
	if a.RunTUI != nil {
		return a.RunTUI(baseURL, timeout, stdout, stderr)
	}
	return runTUI(a.withBaseURL(baseURL), baseURL, timeout, stdout, stderr)
}

func (a App) runDoctor(baseURL string, timeout time.Duration, jsonOutput bool, stdout io.Writer, stderr io.Writer) int {
	result := doctorResult{
		Schema:     JSONSchema,
		Command:    "doctor",
		CLIVersion: Version,
		BaseURL:    baseURL,
	}

	output, err := a.request(baseURL, boardRequest{Method: http.MethodGet, Path: "/api/v1/status"}, timeout)
	if err != nil {
		result.Error = &JSONError{Code: "status_failed", Message: err.Error()}
		return finishDoctor(stdout, stderr, result, jsonOutput, 1)
	}

	cleaned := strings.TrimSpace(output)
	filtered, err := filterInternalPowerOutput(cleaned)
	if err != nil {
		result.Error = &JSONError{Code: "invalid_json", Message: err.Error()}
		return finishDoctor(stdout, stderr, result, jsonOutput, 1)
	}
	cleaned = filtered
	env, err := parseAgentJSON(cleaned, true)
	if err != nil {
		var validationErr jsonValidationError
		if errors.As(err, &validationErr) {
			result.Error = &JSONError{Code: validationErr.code, Message: validationErr.message}
		} else {
			result.Error = &JSONError{Code: "invalid_json", Message: err.Error()}
		}
		result.StatusText = cleaned
		return finishDoctor(stdout, stderr, result, jsonOutput, 1)
	}

	result.Status = json.RawMessage(cleaned)
	if env.OK != nil && !*env.OK {
		result.Error = env.Error
		if result.Error == nil {
			result.Error = &JSONError{Code: "status_error", Message: "board returned ok=false"}
		}
		return finishDoctor(stdout, stderr, result, jsonOutput, 1)
	}

	result.ProbeOK = true
	result.OK = true
	return finishDoctor(stdout, stderr, result, jsonOutput, 0)
}

func finishDoctor(stdout io.Writer, stderr io.Writer, result doctorResult, jsonOutput bool, exitCode int) int {
	if jsonOutput {
		writeJSON(stdout, result)
		return exitCode
	}
	printDoctorText(stdout, result)
	if result.Error != nil && exitCode != 0 {
		fmt.Fprintf(stderr, "%s: %s\n", result.Error.Code, result.Error.Message)
	}
	return exitCode
}

func printDoctorText(w io.Writer, result doctorResult) {
	fmt.Fprintln(w, "Radxa Linkr Debugger doctor")
	fmt.Fprintf(w, "cli_version=%s\n", result.CLIVersion)
	fmt.Fprintf(w, "base_url=%s\n", result.BaseURL)
	fmt.Fprintf(w, "probe=%s\n", mapBool(result.ProbeOK, "ok", "failed"))
	if len(result.Status) > 0 {
		fmt.Fprintf(w, "status=%s\n", string(result.Status))
	}
	if result.StatusText != "" {
		fmt.Fprintf(w, "status_text=%s\n", result.StatusText)
	}
	if result.Error != nil {
		fmt.Fprintf(w, "error=%s: %s\n", result.Error.Code, result.Error.Message)
	} else {
		fmt.Fprintln(w, "result=ok")
	}
}

func mapBool(value bool, trueText string, falseText string) string {
	if value {
		return trueText
	}
	return falseText
}

func IsDebugBoardStatus(output string) bool {
	if strings.Contains(output, projectStatusLine) {
		return true
	}
	var status struct {
		Schema  string `json:"schema"`
		OK      bool   `json:"ok"`
		Command string `json:"command"`
		Project string `json:"project"`
	}
	if err := json.Unmarshal([]byte(strings.TrimSpace(output)), &status); err != nil {
		return false
	}
	return status.Schema == JSONSchema && status.OK && status.Command == "status" && status.Project == "radxa-linkr-debugger"
}

type HTTPClient struct {
	BaseURL string
	Client  *http.Client
}

func (c HTTPClient) Do(ctx context.Context, request boardRequest) ([]byte, error) {
	base := resolveBaseURL(c.BaseURL)
	u, err := url.Parse(base)
	if err != nil {
		return nil, fmt.Errorf("parse base URL %q: %w", base, err)
	}
	path, err := url.JoinPath(strings.TrimRight(u.Path, "/"), request.Path)
	if err != nil {
		return nil, fmt.Errorf("build request path: %w", err)
	}
	u.Path = path
	if len(request.Query) > 0 {
		u.RawQuery = request.Query.Encode()
	}

	var body io.Reader
	if request.Body != nil {
		var encoded bytes.Buffer
		if err := json.NewEncoder(&encoded).Encode(request.Body); err != nil {
			return nil, fmt.Errorf("encode request body: %w", err)
		}
		body = &encoded
	}

	httpReq, err := http.NewRequestWithContext(ctx, request.Method, u.String(), body)
	if err != nil {
		return nil, fmt.Errorf("build request: %w", err)
	}
	httpReq.Header.Set("Accept", "application/json")
	if request.Body != nil {
		httpReq.Header.Set("Content-Type", "application/json")
	}

	client := c.Client
	if client == nil {
		client = http.DefaultClient
	}
	httpResp, err := client.Do(httpReq)
	if err != nil {
		return nil, err
	}
	defer httpResp.Body.Close()

	data, err := io.ReadAll(httpResp.Body)
	if err != nil {
		return nil, fmt.Errorf("read response body: %w", err)
	}
	if httpResp.StatusCode < 200 || httpResp.StatusCode >= 300 {
		message := strings.TrimSpace(string(data))
		if message == "" {
			message = httpResp.Status
		}
		return nil, fmt.Errorf("HTTP %s: %s", httpResp.Status, message)
	}
	return data, nil
}

func CleanOutput(output string, command string) string {
	lines := make([]string, 0)
	normalized := strings.ReplaceAll(ansiRE.ReplaceAllString(output, ""), "\r", "")
	for _, line := range strings.Split(normalized, "\n") {
		stripped := strings.TrimSpace(line)
		if stripped == "" || stripped == command || stripped == PromptText {
			continue
		}
		if strings.HasSuffix(stripped, PromptText) {
			stripped = strings.TrimSpace(strings.TrimSuffix(stripped, PromptText))
		}
		if stripped != "" {
			lines = append(lines, stripped)
		}
	}
	return strings.Join(lines, "\n")
}

func (a App) request(baseURL string, request boardRequest, timeout time.Duration) (string, error) {
	client := a.withBaseURL(baseURL).Client
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	data, err := client.Do(ctx, request)
	if err != nil {
		return "", err
	}
	return string(data), nil
}

func (a App) withBaseURL(baseURL string) App {
	if a.Client == nil {
		a.Client = HTTPClient{BaseURL: baseURL, Client: http.DefaultClient}
		return a
	}
	if client, ok := a.Client.(HTTPClient); ok {
		client.BaseURL = baseURL
		a.Client = client
	}
	return a
}

func hasArg(args []string, value string) bool {
	for _, arg := range args {
		if arg == value {
			return true
		}
	}
	return false
}

func resolveBaseURL(value string) string {
	trimmed := strings.TrimSpace(value)
	if trimmed == "" {
		return DefaultBaseURL
	}
	if strings.HasPrefix(trimmed, "http://") || strings.HasPrefix(trimmed, "https://") {
		return strings.TrimRight(trimmed, "/")
	}
	return "http://" + strings.TrimRight(trimmed, "/")
}

func looksLikeJSON(output string) bool {
	trimmed := strings.TrimSpace(output)
	return strings.HasPrefix(trimmed, "{") || strings.HasPrefix(trimmed, "[")
}

func requestFromArgs(args []string) (boardRequest, string, error) {
	cleaned := stripPassthroughFlags(args)
	if len(cleaned) == 0 {
		return boardRequest{}, "", errors.New("missing command")
	}

	switch cleaned[0] {
	case "status":
		if len(cleaned) != 1 {
			return boardRequest{}, "status", errors.New("usage: radxa-linkr-debuggerctl status")
		}
		return boardRequest{Method: http.MethodGet, Path: "/api/v1/status"}, "debugboard status --json", nil
	case "power":
		return powerRequest(cleaned)
	case "switch":
		return switchRequest(cleaned)
	case "adc":
		return adcRequest(cleaned)
	case "gpio":
		return gpioRequest(cleaned)
	case "watchdog":
		return watchdogRequest(cleaned)
	case "bootloader":
		if len(cleaned) != 1 {
			return boardRequest{}, "bootloader", errors.New("usage: radxa-linkr-debuggerctl bootloader")
		}
		return boardRequest{Method: http.MethodPost, Path: "/api/v1/bootloader"}, "debugboard bootloader --json", nil
	default:
		return boardRequest{}, cleaned[0], fmt.Errorf("unsupported command %q over HTTP", cleaned[0])
	}
}

func watchdogRequest(args []string) (boardRequest, string, error) {
	command := "debugboard " + strings.Join(args, " ") + " --json"
	if len(args) != 2 {
		return boardRequest{}, "watchdog", errors.New("usage: radxa-linkr-debuggerctl watchdog status")
	}
	switch args[1] {
	case "status":
		return boardRequest{Method: http.MethodGet, Path: "/api/v1/watchdog"}, command, nil
	default:
		return boardRequest{}, "watchdog", fmt.Errorf("unsupported watchdog action %q (watchdog is supervised by firmware)", args[1])
	}
}

func stripPassthroughFlags(args []string) []string {
	cleaned := make([]string, 0, len(args))
	for _, arg := range args {
		switch arg {
		case "--json", "-v", "--verbose":
			continue
		default:
			cleaned = append(cleaned, arg)
		}
	}
	return cleaned
}

func powerRequest(args []string) (boardRequest, string, error) {
	commandName := args[0]
	command := "debugboard power " + strings.Join(args[1:], " ") + " --json"
	if len(args) < 2 {
		return boardRequest{}, commandName, errors.New("usage: radxa-linkr-debuggerctl power list|get|set ...")
	}
	switch args[1] {
	case "list":
		if len(args) != 2 {
			return boardRequest{}, commandName, errors.New("usage: radxa-linkr-debuggerctl power list")
		}
		return boardRequest{Method: http.MethodGet, Path: "/api/v1/power"}, command, nil
	case "get":
		if len(args) != 3 {
			return boardRequest{}, commandName, errors.New("usage: radxa-linkr-debuggerctl power get NAME")
		}
		if err := rejectInternalPowerOutput(args[2]); err != nil {
			return boardRequest{}, commandName, err
		}
		return boardRequest{Method: http.MethodGet, Path: "/api/v1/power/" + args[2]}, command, nil
	case "set":
		if len(args) != 4 {
			return boardRequest{}, commandName, errors.New("usage: radxa-linkr-debuggerctl power set NAME on|off")
		}
		if err := rejectInternalPowerOutput(args[2]); err != nil {
			return boardRequest{}, commandName, err
		}
		return boardRequest{Method: http.MethodPut, Path: "/api/v1/power/" + args[2], Body: map[string]string{"state": args[3]}}, command, nil
	default:
		return boardRequest{}, commandName, fmt.Errorf("unsupported power action %q", args[1])
	}
}

func rejectInternalPowerOutput(name string) error {
	if name == internalPowerOutput {
		return fmt.Errorf("power output %q is internal and unavailable through the CLI", name)
	}
	return nil
}

func filterInternalPowerOutput(output string) (string, error) {
	if !strings.Contains(output, internalPowerOutput) {
		return output, nil
	}

	var value any
	if err := json.Unmarshal([]byte(output), &value); err != nil {
		return "", err
	}
	if !removeInternalPowerOutput(value) {
		return output, nil
	}
	filtered, err := json.Marshal(value)
	if err != nil {
		return "", err
	}
	return string(filtered), nil
}

func removeInternalPowerOutput(value any) bool {
	changed := false
	switch typed := value.(type) {
	case map[string]any:
		if rawOutputs, ok := typed["power_outputs"].([]any); ok {
			outputs := rawOutputs[:0]
			for _, output := range rawOutputs {
				item, ok := output.(map[string]any)
				if ok && item["name"] == internalPowerOutput {
					changed = true
					continue
				}
				outputs = append(outputs, output)
			}
			typed["power_outputs"] = outputs
		}
		for _, child := range typed {
			changed = removeInternalPowerOutput(child) || changed
		}
	case []any:
		for _, child := range typed {
			changed = removeInternalPowerOutput(child) || changed
		}
	}
	return changed
}

func adcRequest(args []string) (boardRequest, string, error) {
	command := "debugboard " + strings.Join(args, " ") + " --json"
	if len(args) < 2 || args[1] != "read" {
		return boardRequest{}, "adc", errors.New("usage: radxa-linkr-debuggerctl adc read [NAME]")
	}
	if len(args) > 3 {
		return boardRequest{}, "adc", errors.New("usage: radxa-linkr-debuggerctl adc read [NAME]")
	}
	query := url.Values{}
	if len(args) == 3 {
		query.Set("channel", args[2])
	}
	return boardRequest{Method: http.MethodGet, Path: "/api/v1/adc/read", Query: query}, command, nil
}

func switchRequest(args []string) (boardRequest, string, error) {
	command := "debugboard " + strings.Join(args, " ") + " --json"
	if len(args) < 2 {
		return boardRequest{}, "switch", errors.New("usage: radxa-linkr-debuggerctl switch list|get|route ...")
	}
	switch args[1] {
	case "list":
		if len(args) != 2 {
			return boardRequest{}, "switch", errors.New("usage: radxa-linkr-debuggerctl switch list")
		}
		return boardRequest{Method: http.MethodGet, Path: "/api/v1/switch"}, command, nil
	case "get":
		if len(args) != 3 {
			return boardRequest{}, "switch", errors.New("usage: radxa-linkr-debuggerctl switch get sd|usb")
		}
		return boardRequest{Method: http.MethodGet, Path: "/api/v1/switch/" + args[2]}, command, nil
	case "route":
		if len(args) != 4 {
			return boardRequest{}, "switch", errors.New("usage: radxa-linkr-debuggerctl switch route sd|usb ROUTE")
		}
		return boardRequest{Method: http.MethodPut, Path: "/api/v1/switch/" + args[2], Body: map[string]string{"route": args[3]}}, command, nil
	default:
		return boardRequest{}, "switch", fmt.Errorf("unsupported switch action %q", args[1])
	}
}

func gpioRequest(args []string) (boardRequest, string, error) {
	command := "debugboard " + strings.Join(args, " ") + " --json"
	if len(args) < 2 {
		return boardRequest{}, "gpio", errors.New("usage: radxa-linkr-debuggerctl gpio list|set|input ...")
	}
	switch args[1] {
	case "list":
		if len(args) != 2 {
			return boardRequest{}, "gpio", errors.New("usage: radxa-linkr-debuggerctl gpio list")
		}
		return boardRequest{Method: http.MethodGet, Path: "/api/v1/gpio"}, command, nil
	case "input":
		if len(args) != 3 {
			return boardRequest{}, "gpio", errors.New("usage: radxa-linkr-debuggerctl gpio input NAME")
		}
		return boardRequest{Method: http.MethodPut, Path: "/api/v1/gpio/" + args[2], Body: map[string]string{"direction": "input"}}, command, nil
	case "set":
		if len(args) != 4 {
			return boardRequest{}, "gpio", errors.New("usage: radxa-linkr-debuggerctl gpio set NAME 0|1")
		}
		return boardRequest{Method: http.MethodPut, Path: "/api/v1/gpio/" + args[2], Body: map[string]any{"direction": "output", "value": mustParseInt(args[3])}}, command, nil
	default:
		return boardRequest{}, "gpio", fmt.Errorf("unsupported gpio action %q", args[1])
	}
}

func mustParseInt(value string) int {
	parsed, err := strconv.Atoi(value)
	if err != nil {
		return 0
	}

	return parsed
}

func commandName(args []string, raw bool) string {
	if raw {
		return "raw"
	}
	if len(args) == 0 {
		return "radxa-linkr-debuggerctl"
	}
	return args[0]
}

func writeValidatedJSON(w io.Writer, output string, command string, requireEnvelope bool) int {
	env, err := parseAgentJSON(output, requireEnvelope)
	if err != nil {
		var validationErr jsonValidationError
		if errors.As(err, &validationErr) {
			writeJSONError(w, command, validationErr.code, validationErr.message)
		} else {
			writeJSONError(w, command, "invalid_json", err.Error())
		}
		return 1
	}

	fmt.Fprintln(w, output)
	if requireEnvelope && env.OK != nil && !*env.OK {
		return 1
	}
	return 0
}

func parseAgentJSON(output string, requireEnvelope bool) (*jsonEnvelope, error) {
	cleaned := strings.TrimSpace(output)
	if cleaned == "" {
		return nil, jsonValidationError{
			code:    "invalid_json",
			message: "firmware returned empty output",
		}
	}
	if !json.Valid([]byte(cleaned)) {
		return nil, jsonValidationError{
			code:    "invalid_json",
			message: "firmware returned non-JSON output",
		}
	}
	if !requireEnvelope {
		return &jsonEnvelope{}, nil
	}

	var env jsonEnvelope
	if err := json.Unmarshal([]byte(cleaned), &env); err != nil {
		return nil, jsonValidationError{
			code:    "invalid_json",
			message: err.Error(),
		}
	}

	if requireEnvelope {
		if env.Schema != JSONSchema || env.OK == nil || env.Command == "" {
			return nil, jsonValidationError{
				code:    "invalid_json",
				message: "firmware returned JSON without radxa-linkr-debugger.v1 envelope",
			}
		}
	}

	return &env, nil
}

func writeJSONError(w io.Writer, command string, code string, message string) {
	writeJSON(w, jsonResponse{
		Schema:  JSONSchema,
		OK:      false,
		Command: command,
		Error: &JSONError{
			Code:    code,
			Message: message,
		},
	})
}

func writeJSON(w io.Writer, value any) {
	encoder := json.NewEncoder(w)
	encoder.SetEscapeHTML(false)
	_ = encoder.Encode(value)
}
