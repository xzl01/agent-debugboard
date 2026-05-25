package hostcli

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"strings"
)

type adcSensorValue struct {
	Val1 int32 `json:"val1"`
	Val2 int32 `json:"val2"`
}

type adcReading struct {
	Name          string          `json:"name"`
	Signal        string          `json:"signal,omitempty"`
	Raw           *int32          `json:"raw"`
	CurrentValid  *bool           `json:"current_valid,omitempty"`
	MV            *int32          `json:"mv,omitempty"`
	MAEst         *int32          `json:"ma_est,omitempty"`
	PowerEnabled  *bool           `json:"power_enabled,omitempty"`
	SensorChannel string          `json:"sensor_channel,omitempty"`
	Unit          string          `json:"unit,omitempty"`
	SensorValue   *adcSensorValue `json:"sensor_value,omitempty"`
	CurrentUA     *int32          `json:"current_ua,omitempty"`
}

type adcResponse struct {
	Schema   string       `json:"schema"`
	OK       bool         `json:"ok"`
	Command  string       `json:"command"`
	Action   string       `json:"action,omitempty"`
	Readings []adcReading `json:"readings,omitempty"`
	Error    *JSONError   `json:"error,omitempty"`
}

func isADCReadCommand(args []string) bool {
	return len(args) >= 1 && args[0] == "adc" && (len(args) == 1 || args[1] == "read")
}

func int32Ptr(value int32) *int32 {
	return &value
}

func boolPtr(value bool) *bool {
	return &value
}

func sensorValueToMicroamps(value adcSensorValue) int32 {
	return value.Val1*1000000 + value.Val2
}

func sensorValueFromMicroamps(currentUA int32) adcSensorValue {
	return adcSensorValue{Val1: currentUA / 1000000, Val2: currentUA % 1000000}
}

func currentTextFromMicroamps(currentUA int32) string {
	absUA := currentUA
	prefix := ""
	if absUA < 0 {
		prefix = "-"
		absUA = -absUA
	}
	return fmt.Sprintf("%s%d.%06dA", prefix, absUA/1000000, absUA%1000000)
}

func transformADCReading(reading adcReading) (adcReading, error) {
	var currentUA int32
	switch {
	case reading.CurrentUA != nil:
		currentUA = *reading.CurrentUA
	case reading.SensorValue != nil:
		currentUA = sensorValueToMicroamps(*reading.SensorValue)
	default:
		return reading, nil
	}

	reading.CurrentUA = int32Ptr(currentUA)
	if reading.SensorValue == nil {
		sensorValue := sensorValueFromMicroamps(currentUA)
		reading.SensorValue = &sensorValue
	}
	if reading.SensorChannel == "" {
		reading.SensorChannel = "current"
	}
	if reading.Unit == "" {
		reading.Unit = "A"
	}

	return reading, nil
}

func transformADCResponse(output string) (*adcResponse, error) {
	var response adcResponse
	if err := json.Unmarshal([]byte(output), &response); err != nil {
		return nil, err
	}

	transformed := make([]adcReading, 0, len(response.Readings))
	for _, reading := range response.Readings {
		enriched, err := transformADCReading(reading)
		if err != nil {
			return nil, err
		}
		transformed = append(transformed, enriched)
	}

	response.Readings = transformed
	return &response, nil
}

func writeADCReadJSON(w io.Writer, output string, command string) int {
	env, err := parseAgentJSON(output, true)
	if err != nil {
		var validationErr jsonValidationError
		if errors.As(err, &validationErr) {
			writeJSONError(w, command, validationErr.code, validationErr.message)
		} else {
			writeJSONError(w, command, "invalid_json", err.Error())
		}
		return 1
	}
	if env.OK != nil && !*env.OK {
		fmt.Fprintln(w, output)
		return 1
	}

	response, err := transformADCResponse(output)
	if err != nil {
		writeJSONError(w, command, "invalid_json", err.Error())
		return 1
	}
	writeJSON(w, response)
	return 0
}

func writeADCReadText(stdout io.Writer, stderr io.Writer, output string, verbose bool) int {
	env, err := parseAgentJSON(output, true)
	if err != nil {
		fmt.Fprintln(stderr, err)
		return 1
	}
	if env.OK != nil && !*env.OK {
		if env.Error != nil {
			fmt.Fprintf(stderr, "%s: %s\n", env.Error.Code, env.Error.Message)
		} else {
			fmt.Fprintln(stderr, "board returned adc error")
		}
		return 1
	}

	response, err := transformADCResponse(output)
	if err != nil {
		fmt.Fprintln(stderr, err)
		return 1
	}

	for _, reading := range response.Readings {
		if verbose {
			parts := []string{reading.Name}
			if reading.Signal != "" {
				parts = append(parts, "signal="+reading.Signal)
			}
			if reading.PowerEnabled != nil {
				parts = append(parts, "power="+map[bool]string{true: "on", false: "off"}[*reading.PowerEnabled])
			}
			if reading.CurrentUA != nil {
				parts = append(parts, "current="+currentTextFromMicroamps(*reading.CurrentUA))
				parts = append(parts, fmt.Sprintf("current_ua=%d", *reading.CurrentUA))
			}
			if reading.Raw != nil {
				parts = append(parts, fmt.Sprintf("raw=%d", *reading.Raw))
			} else {
				parts = append(parts, "raw=null")
			}
			if reading.MV != nil {
				parts = append(parts, fmt.Sprintf("mv=%d", *reading.MV))
			}
			if reading.MAEst != nil {
				parts = append(parts, fmt.Sprintf("ma_est=%d", *reading.MAEst))
			}
			fmt.Fprintln(stdout, strings.Join(parts, " "))
			continue
		}

		if reading.CurrentUA != nil {
			fmt.Fprintf(stdout, "%s=%s\n", reading.Name, currentTextFromMicroamps(*reading.CurrentUA))
			continue
		}
		if reading.MAEst != nil {
			fmt.Fprintf(stdout, "%s=%dmA\n", reading.Name, *reading.MAEst)
		}
	}

	return 0
}
