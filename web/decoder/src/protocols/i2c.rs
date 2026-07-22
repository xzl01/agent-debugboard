// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::annotation::{annotation, diagnostic, DiagnosticSeverity};
use crate::input::{bit_at, channel_bit, PackedSamples};
use crate::protocols::I2cOptions;
use crate::schema::{DecodeError, DecodeResult};
use serde_json::json;

pub fn decode(
    samples: &PackedSamples,
    options: &I2cOptions,
    result: &mut DecodeResult,
) -> Result<(), DecodeError> {
    let scl = channel_bit(samples, &options.scl)?;
    let sda = channel_bit(samples, &options.sda)?;
    let mut in_transfer = false;
    let mut bits: Vec<(usize, bool)> = Vec::new();
    let mut byte_index = 0usize;
    let mut byte_start = 0usize;

    for i in 1..samples.sample_count {
        let prev_scl = bit_at(samples, i - 1, scl);
        let prev_sda = bit_at(samples, i - 1, sda);
        let cur_scl = bit_at(samples, i, scl);
        let cur_sda = bit_at(samples, i, sda);

        if prev_scl && cur_scl && prev_sda && !cur_sda {
            let class = if in_transfer { "restart" } else { "start" };
            result.annotations.push(annotation(
                i,
                i + 1,
                "i2c",
                class,
                if in_transfer { "Sr" } else { "S" }.to_string(),
                if in_transfer {
                    "I2C repeated START"
                } else {
                    "I2C START"
                }
                .to_string(),
                json!({ "protocol": "i2c", "event": class }),
            ));
            in_transfer = true;
            bits.clear();
            byte_index = 0;
            byte_start = i;
            continue;
        }

        if prev_scl && cur_scl && !prev_sda && cur_sda {
            result.annotations.push(annotation(
                i,
                i + 1,
                "i2c",
                "stop",
                "P".to_string(),
                "I2C STOP".to_string(),
                json!({ "protocol": "i2c", "event": "stop" }),
            ));
            in_transfer = false;
            bits.clear();
            continue;
        }

        if in_transfer && !prev_scl && cur_scl {
            if bits.is_empty() {
                byte_start = i;
            }
            bits.push((i, cur_sda));
            if bits.len() == 9 {
                emit_byte(result, &bits, byte_start, byte_index);
                byte_index += 1;
                bits.clear();
            }
        }
    }

    if in_transfer && !bits.is_empty() {
        result.diagnostics.push(diagnostic(
            byte_start,
            samples.sample_count,
            DiagnosticSeverity::Warning,
            "truncated-i2c-byte",
            "capture ended before a complete I2C byte and ACK were available",
        ));
    }

    Ok(())
}

fn emit_byte(
    result: &mut DecodeResult,
    bits: &[(usize, bool)],
    byte_start: usize,
    byte_index: usize,
) {
    let mut value = 0u8;
    for (bit_index, (_, level)) in bits.iter().take(8).enumerate() {
        if *level {
            value |= 1 << (7 - bit_index);
        }
    }
    let ack = !bits[8].1;
    if byte_index == 0 {
        let address = value >> 1;
        let read = (value & 1) != 0;
        result.annotations.push(annotation(
            byte_start,
            bits[8].0 + 1,
            "i2c",
            "address",
            format!(
                "0x{address:02X} {} {}",
                if read { "R" } else { "W" },
                ack_text(ack)
            ),
            format!(
                "I2C address 0x{address:02X} {} {}",
                if read { "read" } else { "write" },
                ack_text(ack)
            ),
            json!({ "protocol": "i2c", "address": address, "read": read, "ack": ack }),
        ));
    } else {
        result.annotations.push(annotation(
            byte_start,
            bits[8].0 + 1,
            "i2c",
            "data",
            format!("0x{value:02X} {}", ack_text(ack)),
            format!("I2C data 0x{value:02X} {}", ack_text(ack)),
            json!({ "protocol": "i2c", "value": value, "ack": ack }),
        ));
    }
}

fn ack_text(ack: bool) -> &'static str {
    if ack {
        "ACK"
    } else {
        "NACK"
    }
}
