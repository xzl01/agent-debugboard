// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::annotation::{annotation, diagnostic, DiagnosticSeverity};
use crate::input::{bit_at, channel_bit, PackedSamples};
use crate::protocols::{BitOrder, SpiOptions};
use crate::schema::{DecodeError, DecodeResult};
use serde_json::{json, Value};

pub fn decode(
    samples: &PackedSamples,
    options: &SpiOptions,
    result: &mut DecodeResult,
) -> Result<(), DecodeError> {
    if options.mode > 3 {
        return Err(DecodeError::InvalidOptions(
            "SPI mode must be in 0..=3".to_string(),
        ));
    }
    if options.bits_per_word == 0 || options.bits_per_word > 16 {
        return Err(DecodeError::InvalidOptions(
            "SPI bits per word must be in 1..=16".to_string(),
        ));
    }
    if options.mosi.is_none() && options.miso.is_none() {
        return Err(DecodeError::InvalidOptions(
            "SPI requires at least MOSI or MISO".to_string(),
        ));
    }

    let sclk = channel_bit(samples, &options.sclk)?;
    let mosi = options
        .mosi
        .as_deref()
        .map(|name| channel_bit(samples, name))
        .transpose()?;
    let miso = options
        .miso
        .as_deref()
        .map(|name| channel_bit(samples, name))
        .transpose()?;
    let cs = options
        .cs
        .as_deref()
        .map(|name| channel_bit(samples, name))
        .transpose()?;
    let cpol = options.mode >= 2;
    let cpha = options.mode % 2 == 1;
    let mut bits_seen = 0u8;
    let mut word_start = 0usize;
    let mut mosi_word = 0u16;
    let mut miso_word = 0u16;

    for i in 1..samples.sample_count {
        if !selected(samples, i, cs, options.cs_active_high) {
            if bits_seen != 0 {
                result.diagnostics.push(diagnostic(
                    word_start,
                    i,
                    DiagnosticSeverity::Warning,
                    "truncated-spi-word",
                    "chip select deasserted before a complete SPI word was available",
                ));
                bits_seen = 0;
            }
            continue;
        }

        let prev_clk = bit_at(samples, i - 1, sclk);
        let cur_clk = bit_at(samples, i, sclk);
        if prev_clk == cur_clk {
            continue;
        }
        let leading = prev_clk == cpol && cur_clk != cpol;
        let sample_edge = if cpha { !leading } else { leading };
        if !sample_edge {
            continue;
        }
        if bits_seen == 0 {
            word_start = i;
            mosi_word = 0;
            miso_word = 0;
        }
        if let Some(bit) = mosi {
            push_bit(&mut mosi_word, bit_at(samples, i, bit), bits_seen, options);
        }
        if let Some(bit) = miso {
            push_bit(&mut miso_word, bit_at(samples, i, bit), bits_seen, options);
        }
        bits_seen += 1;
        if bits_seen == options.bits_per_word {
            emit_word(
                result,
                DecodedWord {
                    start_sample: word_start,
                    end_sample: i + 1,
                    mosi,
                    miso,
                    mosi_word,
                    miso_word,
                    bits_per_word: options.bits_per_word,
                },
            );
            bits_seen = 0;
        }
    }

    if bits_seen != 0 {
        result.diagnostics.push(diagnostic(
            word_start,
            samples.sample_count,
            DiagnosticSeverity::Warning,
            "truncated-spi-word",
            "capture ended before a complete SPI word was available",
        ));
    }

    Ok(())
}

fn selected(samples: &PackedSamples, sample: usize, cs: Option<u8>, active_high: bool) -> bool {
    match cs {
        Some(bit) => bit_at(samples, sample, bit) == active_high,
        None => true,
    }
}

fn push_bit(word: &mut u16, level: bool, bit_index: u8, options: &SpiOptions) {
    if !level {
        return;
    }
    match options.bit_order {
        BitOrder::MsbFirst => *word |= 1 << (options.bits_per_word - 1 - bit_index),
        BitOrder::LsbFirst => *word |= 1 << bit_index,
    }
}

struct DecodedWord {
    start_sample: usize,
    end_sample: usize,
    mosi: Option<u8>,
    miso: Option<u8>,
    mosi_word: u16,
    miso_word: u16,
    bits_per_word: u8,
}

fn emit_word(result: &mut DecodeResult, word: DecodedWord) {
    let mut data = json!({ "protocol": "spi", "bitsPerWord": word.bits_per_word });
    if let Value::Object(ref mut object) = data {
        if word.mosi.is_some() {
            object.insert("mosi".to_string(), json!(word.mosi_word));
        }
        if word.miso.is_some() {
            object.insert("miso".to_string(), json!(word.miso_word));
        }
    }
    let short_text = match (word.mosi.is_some(), word.miso.is_some()) {
        (true, true) => format!("MOSI 0x{:X} MISO 0x{:X}", word.mosi_word, word.miso_word),
        (true, false) => format!("MOSI 0x{:X}", word.mosi_word),
        (false, true) => format!("MISO 0x{:X}", word.miso_word),
        (false, false) => unreachable!(),
    };
    result.annotations.push(annotation(
        word.start_sample,
        word.end_sample,
        "spi",
        "word",
        short_text.clone(),
        format!("SPI {short_text}"),
        data,
    ));
}
