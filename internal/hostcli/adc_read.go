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

type adcOffsetPoint struct {
	NominalMA int32
	OffsetMA  int32
}

type adcChannelModel struct {
	Name         string
	Signal       string
	MAPerMV      int32
	OffsetPoints []adcOffsetPoint
}

var fiveVoltOffsetPoints = []adcOffsetPoint{
	{NominalMA: 550, OffsetMA: -550},
	{NominalMA: 600, OffsetMA: -400},
	{NominalMA: 650, OffsetMA: -350},
	{NominalMA: 800, OffsetMA: -400},
	{NominalMA: 850, OffsetMA: -350},
	{NominalMA: 1000, OffsetMA: -400},
	{NominalMA: 1050, OffsetMA: -350},
	{NominalMA: 1200, OffsetMA: -400},
	{NominalMA: 1300, OffsetMA: -400},
	{NominalMA: 1350, OffsetMA: -350},
	{NominalMA: 1450, OffsetMA: -350},
	{NominalMA: 1650, OffsetMA: -400},
	{NominalMA: 1800, OffsetMA: -400},
	{NominalMA: 1900, OffsetMA: -400},
	{NominalMA: 2000, OffsetMA: -400},
	{NominalMA: 2050, OffsetMA: -350},
	{NominalMA: 2150, OffsetMA: -350},
	{NominalMA: 2300, OffsetMA: -400},
	{NominalMA: 2400, OffsetMA: -400},
	{NominalMA: 2450, OffsetMA: -350},
	{NominalMA: 2600, OffsetMA: -400},
	{NominalMA: 2650, OffsetMA: -350},
	{NominalMA: 2800, OffsetMA: -400},
	{NominalMA: 2900, OffsetMA: -400},
	{NominalMA: 3050, OffsetMA: -450},
	{NominalMA: 3100, OffsetMA: -400},
	{NominalMA: 3250, OffsetMA: -450},
	{NominalMA: 3300, OffsetMA: -400},
	{NominalMA: 3400, OffsetMA: -400},
	{NominalMA: 3500, OffsetMA: -400},
	{NominalMA: 3700, OffsetMA: -400},
	{NominalMA: 3850, OffsetMA: -450},
	{NominalMA: 3900, OffsetMA: -400},
	{NominalMA: 4050, OffsetMA: -450},
	{NominalMA: 4100, OffsetMA: -400},
	{NominalMA: 4300, OffsetMA: -500},
	{NominalMA: 4350, OffsetMA: -450},
	{NominalMA: 4450, OffsetMA: -450},
	{NominalMA: 4550, OffsetMA: -450},
	{NominalMA: 4600, OffsetMA: -400},
	{NominalMA: 4750, OffsetMA: -450},
}

var adcChannelModels = map[string]adcChannelModel{
	"5v_out": {
		Name:         "5v_out",
		Signal:       "S_C_5V",
		MAPerMV:      50,
		OffsetPoints: fiveVoltOffsetPoints,
	},
	"12v_out": {
		Name:    "12v_out",
		Signal:  "S_C_12V",
		MAPerMV: 50,
	},
	"20v_out": {
		Name:    "20v_out",
		Signal:  "S_C_20V",
		MAPerMV: 50,
	},
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

func nominalMVFromCurrentUA(currentUA int32, model adcChannelModel) int32 {
	nominalMA := currentUA / 1000
	if nominalMA <= 0 || model.MAPerMV == 0 {
		return 0
	}
	return nominalMA / model.MAPerMV
}

func applyCurrentOffset(nominalMA int32, model adcChannelModel) int32 {
	points := model.OffsetPoints
	count := len(points)

	if count > 0 {
		if nominalMA <= points[0].NominalMA {
			adjusted := nominalMA + points[0].OffsetMA
			if adjusted <= 0 {
				return 0
			}
			return adjusted
		}

		for i := 1; i < count; i++ {
			prevNominal := points[i-1].NominalMA
			nextNominal := points[i].NominalMA
			prevOffset := points[i-1].OffsetMA
			nextOffset := points[i].OffsetMA

			if nominalMA > nextNominal {
				continue
			}
			den := nextNominal - prevNominal
			if den <= 0 {
				adjusted := nominalMA + nextOffset
				if adjusted <= 0 {
					return 0
				}
				return adjusted
			}

			num := int64(nominalMA-prevNominal) * int64(nextOffset-prevOffset)
			adjusted := nominalMA + prevOffset + int32((num+int64(den/2))/int64(den))
			if adjusted <= 0 {
				return 0
			}
			return adjusted
		}

		if count >= 2 {
			prevNominal := points[count-2].NominalMA
			nextNominal := points[count-1].NominalMA
			prevOffset := points[count-2].OffsetMA
			nextOffset := points[count-1].OffsetMA
			den := nextNominal - prevNominal
			if den <= 0 {
				adjusted := nominalMA + nextOffset
				if adjusted <= 0 {
					return 0
				}
				return adjusted
			}

			num := int64(nominalMA-nextNominal) * int64(nextOffset-prevOffset)
			adjusted := nominalMA + nextOffset + int32((num+int64(den/2))/int64(den))
			if adjusted <= 0 {
				return 0
			}
			return adjusted
		}

		adjusted := nominalMA + points[count-1].OffsetMA
		if adjusted <= 0 {
			return 0
		}
		return adjusted
	}

	if nominalMA <= 0 {
		return 0
	}
	return nominalMA
}

func findADCChannelModel(reading adcReading) (adcChannelModel, bool) {
	if model, ok := adcChannelModels[reading.Name]; ok {
		return model, true
	}
	for _, model := range adcChannelModels {
		if model.Signal == reading.Signal {
			return model, true
		}
	}
	return adcChannelModel{}, false
}

func transformADCReading(reading adcReading) (adcReading, error) {
	if reading.MAEst != nil {
		return reading, nil
	}

	model, ok := findADCChannelModel(reading)
	if !ok {
		return adcReading{}, fmt.Errorf("unknown ADC channel %q", reading.Name)
	}

	var currentUA int32
	switch {
	case reading.CurrentUA != nil:
		currentUA = *reading.CurrentUA
	case reading.SensorValue != nil:
		currentUA = sensorValueToMicroamps(*reading.SensorValue)
	default:
			return adcReading{}, fmt.Errorf("reading %q missing current value", reading.Name)
	}

	nominalMA := currentUA / 1000
	mv := nominalMVFromCurrentUA(currentUA, model)
	maEst := applyCurrentOffset(nominalMA, model)
	if reading.PowerEnabled != nil && !*reading.PowerEnabled {
		maEst = 0
	}

	reading.Raw = nil
	reading.CurrentValid = boolPtr(true)
	reading.MV = int32Ptr(mv)
	reading.MAEst = int32Ptr(maEst)
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
			parts = append(parts, "raw=null")
			if reading.MV != nil {
				parts = append(parts, fmt.Sprintf("mv=%d", *reading.MV))
			}
			if reading.MAEst != nil {
				parts = append(parts, fmt.Sprintf("ma_est=%d", *reading.MAEst))
			}
			fmt.Fprintln(stdout, strings.Join(parts, " "))
			continue
		}

		if reading.MAEst != nil {
			fmt.Fprintf(stdout, "%s=%dmA\n", reading.Name, *reading.MAEst)
			continue
		}
		if reading.CurrentUA != nil {
			fmt.Fprintf(stdout, "%s=%s\n", reading.Name, currentTextFromMicroamps(*reading.CurrentUA))
		}
	}

	return 0
}
