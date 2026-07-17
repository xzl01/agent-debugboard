// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::annotation::{diagnostic, DiagnosticSeverity};
use crate::input::{bit_at, channel_bit, PackedSamples};
use crate::protocols::{Parity, UartOptions};
use crate::schema::{DecodeError, DecodeResult};
use serde_json::json;

fn decode_frame(
    samples: &PackedSamples,
    options: &UartOptions,
    result: &mut DecodeResult,
    rx: u8,
    bit_samples: f64,
    frame_start: usize,
    frame_end: usize,
) {
    let mut value = 0u16;
    let mut ones = 0u8;
    for bit_index in 0..options.data_bits {
        let sample_at =
            (frame_start as f64 + bit_samples * (1.5 + f64::from(bit_index))).round() as usize;
        if level(samples, sample_at, rx, options.inverted) {
            value |= 1 << bit_index;
            ones += 1;
        }
    }

    let mut cursor = 1 + usize::from(options.data_bits);
    if options.parity != Parity::None {
        let sample_at = (frame_start as f64 + bit_samples * (0.5 + cursor as f64)).round() as usize;
        let parity_level = level(samples, sample_at, rx, options.inverted);
        let expected_high = match options.parity {
            Parity::None => false,
            Parity::Even => (ones & 1) == 1,
            Parity::Odd => (ones & 1) == 0,
        };
        if parity_level != expected_high {
            result.diagnostics.push(diagnostic(
                sample_at,
                sample_at + 1,
                DiagnosticSeverity::Error,
                "uart-parity",
                "UART parity bit did not match decoded data",
            ));
        }
        cursor += 1;
    }

    for _ in 0..options.stop_bits {
        let sample_at = (frame_start as f64 + bit_samples * (0.5 + cursor as f64)).round() as usize;
        if !level(samples, sample_at, rx, options.inverted) {
            result.diagnostics.push(diagnostic(
                sample_at,
                sample_at + 1,
                DiagnosticSeverity::Error,
                "uart-stop-bit",
                "UART stop bit was not idle high",
            ));
        }
        cursor += 1;
    }

    let ascii = if (0x20..=0x7E).contains(&value) {
        format!(" '{}'", value as u8 as char)
    } else {
        String::new()
    };

    result.annotations.push(crate::annotation::annotation(
        frame_start,
        frame_end,
        "uart",
        "data",
        format!("0x{value:02X}{ascii}"),
        format!("UART data 0x{value:02X}{ascii}"),
        json!({ "protocol": "uart", "value": value, "dataBits": options.data_bits }),
    ));
}

pub fn decode(
    samples: &PackedSamples,
    options: &UartOptions,
    result: &mut DecodeResult,
) -> Result<(), DecodeError> {
    if options.baud == 0 {
        return Err(DecodeError::InvalidOptions(
            "UART baud must be positive".to_string(),
        ));
    }
    if !(5..=9).contains(&options.data_bits) {
        return Err(DecodeError::InvalidOptions(
            "UART data bits must be in 5..=9".to_string(),
        ));
    }
    if options.stop_bits == 0 || options.stop_bits > 2 {
        return Err(DecodeError::InvalidOptions(
            "UART stop bits must be 1 or 2".to_string(),
        ));
    }

    let rx = channel_bit(samples, &options.rx)?;
    let bit_samples = samples.sample_rate_hz as f64 / f64::from(options.baud);
    if bit_samples < 2.0 {
        return Err(DecodeError::InvalidOptions(
            "UART baud is too high for the sample rate".to_string(),
        ));
    }

    let parity_bits = usize::from(options.parity != Parity::None);
    let frame_bits =
        1 + usize::from(options.data_bits) + parity_bits + usize::from(options.stop_bits);
    let frame_samples = (bit_samples * frame_bits as f64).ceil() as usize;

    if samples.sample_count >= frame_samples && !level(samples, 0, rx, options.inverted) {
        let start_mid = (bit_samples * 0.5).round() as usize;
        if start_mid < samples.sample_count && !level(samples, start_mid, rx, options.inverted) {
            decode_frame(samples, options, result, rx, bit_samples, 0, frame_samples);
        }
    }

    let mut i = 1usize;
    while i < samples.sample_count {
        let prev = level(samples, i - 1, rx, options.inverted);
        let current = level(samples, i, rx, options.inverted);
        if prev && !current {
            let frame_end = i + frame_samples;
            if frame_end > samples.sample_count {
                result.diagnostics.push(diagnostic(
                    i,
                    samples.sample_count,
                    DiagnosticSeverity::Warning,
                    "truncated-uart-frame",
                    "capture ended before a complete UART frame was available",
                ));
                break;
            }

            let start_mid = (i as f64 + bit_samples * 0.5).round() as usize;
            if start_mid >= samples.sample_count || level(samples, start_mid, rx, options.inverted)
            {
                i += 1;
                continue;
            }

            decode_frame(samples, options, result, rx, bit_samples, i, frame_end);
            let search_from = frame_end.saturating_sub(bit_samples as usize);
            i = search_from.max(i + 1);
        } else {
            i += 1;
        }
    }

    Ok(())
}

fn level(samples: &PackedSamples, sample: usize, bit: u8, inverted: bool) -> bool {
    bit_at(samples, sample, bit) ^ inverted
}
