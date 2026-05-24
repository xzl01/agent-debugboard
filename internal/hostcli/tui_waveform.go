package hostcli

import (
	"fmt"
	"math"
	"strings"

	"github.com/charmbracelet/lipgloss"
)

// brailleBarCells maps a bottom-up fill level (0-4 subrows) to a braille cell
// with both columns filled, giving 4x vertical sub-cell precision per row.
var brailleBarCells = []rune{'·', '⣀', '⣤', '⣶', '⣿'}

const barCurveGamma = 0.35

// nonlinearBarLabel returns the mA value corresponding to a row position on the
// power curve scale. Row 0 is the top (highest), row height-1 is the bottom.
func nonlinearBarLabel(row, height, maxValue int) int32 {
	if row <= 0 {
		return int32(maxValue)
	}
	if row >= height-1 {
		return 0
	}
	ratio := float64(height-row) / float64(height)
	return int32(float64(maxValue) * math.Pow(ratio, 1.0/barCurveGamma))
}

func renderBarChart(title string, currentText string, detailText string, powerState string, series []int32, currentValue int32, width int, height int, maxValue int32) string {
	if width < 14 {
		width = 14
	}
	if height < 3 {
		height = 3
	}
	if maxValue <= 0 {
		maxValue = 1
	}
	if currentValue < 0 {
		currentValue = 0
	}
	if currentValue > maxValue {
		currentValue = maxValue
	}

	contentWidth := width - 2
	if contentWidth < 12 {
		contentWidth = 12
	}

	axisWidth := 3
	plotWidth := contentWidth - axisWidth - 1
	if plotWidth < 4 {
		plotWidth = 4
	}

	historyWidth := plotWidth
	trimmed := series
	if len(trimmed) > historyWidth {
		trimmed = trimmed[len(trimmed)-historyWidth:]
	}
	if len(trimmed) == 0 {
		trimmed = []int32{0}
	}

	leftHeader := fmt.Sprintf("%s %s", title, powerState)
	rightWidth := contentWidth - len(leftHeader) - 1
	if rightWidth < 0 {
		rightWidth = 0
	}
	realtimeText := fmt.Sprintf("%.2fA", float64(currentValue)/1000.0)
	lines := []string{
		leftHeader + lipgloss.NewStyle().AlignHorizontal(lipgloss.Right).Width(rightWidth).Render(realtimeText),
	}

	columns := make([]int, len(trimmed))
	for i, value := range trimmed {
		if value < 0 {
			value = 0
		}
		if value > maxValue {
			value = maxValue
		}
		colUnits := int(math.Round(float64(height*4) * math.Pow(float64(value)/float64(maxValue), barCurveGamma)))
		if value <= 0 {
			colUnits = 0
		}
		if value >= maxValue {
			colUnits = height * 4
		}
		columns[i] = colUnits
	}

	for y := 0; y < height; y++ {
		label := strings.Repeat(" ", axisWidth)
		switch y {
		case 0:
			label = "5A "
		case height - 1:
			label = "0A "
		}

		row := make([]rune, plotWidth)
		for i := range row {
			row[i] = '·'
		}
		rowTopUnits := (height - y - 1) * 4
		start := plotWidth - len(columns)
		for i, colUnits := range columns {
			fillUnits := colUnits - rowTopUnits
			if fillUnits < 0 {
				fillUnits = 0
			}
			if fillUnits > 4 {
				fillUnits = 4
			}
			if fillUnits > 0 {
				row[start+i] = brailleBarCells[fillUnits]
			}
		}

		lines = append(lines, fmt.Sprintf("%s│%s", label, string(row)))
	}

	panel := strings.Join(lines, "\n")
	return lipgloss.NewStyle().Border(lipgloss.RoundedBorder()).Padding(0, 1).Width(width).Render(panel)
}

func joinBlocksHorizontally(blocks []string, gap int) string {
	if len(blocks) == 0 {
		return ""
	}

	gapText := strings.Repeat(" ", gap)
	parsed := make([][]string, 0, len(blocks))
	maxLines := 0
	widths := make([]int, 0, len(blocks))

	for _, block := range blocks {
		lines := strings.Split(block, "\n")
		parsed = append(parsed, lines)
		if len(lines) > maxLines {
			maxLines = len(lines)
		}
		widths = append(widths, lipgloss.Width(block))
	}

	rows := make([]string, 0, maxLines)
	for lineIdx := 0; lineIdx < maxLines; lineIdx++ {
		parts := make([]string, 0, len(parsed))
		for blockIdx, lines := range parsed {
			line := ""
			if lineIdx < len(lines) {
				line = lines[lineIdx]
			}
			parts = append(parts, lipgloss.NewStyle().Width(widths[blockIdx]).Render(line))
		}
		rows = append(rows, strings.Join(parts, gapText))
	}

	return strings.Join(rows, "\n")
}
