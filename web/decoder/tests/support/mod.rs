// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use radxa_logic_decoder::{BitOrder, ChannelMapping, PackedSamples};

pub fn uart_samples(
    value: u16,
    inverted: bool,
    parity: Option<bool>,
    stop_high: bool,
) -> PackedSamples {
    let bit_samples = 10usize;
    let mut logical_bits = vec![true; 2];
    logical_bits.push(false);
    for bit in 0..8 {
        logical_bits.push(((value >> bit) & 1) != 0);
    }
    if let Some(parity_bit) = parity {
        logical_bits.push(parity_bit);
    }
    logical_bits.push(stop_high);
    logical_bits.extend([true, true]);

    let mut words = Vec::new();
    for level in logical_bits {
        let physical = level ^ inverted;
        words.resize(words.len() + bit_samples, u16::from(physical));
    }
    PackedSamples {
        sample_rate_hz: 10_000,
        sample_period_ps: 100_000_000,
        sample_count: words.len(),
        channels: vec![ChannelMapping {
            name: "rx".to_string(),
            bit: 0,
        }],
        words,
    }
}

pub fn i2c_samples(bytes: &[(u8, bool)]) -> PackedSamples {
    let mut words = vec![0b11; 4];
    push_i2c_word(&mut words, true, false);
    push_i2c_word(&mut words, false, false);
    for (byte, ack) in bytes {
        for bit in (0..8).rev() {
            push_i2c_bit(&mut words, ((*byte >> bit) & 1) != 0);
        }
        push_i2c_bit(&mut words, !*ack);
    }
    push_i2c_word(&mut words, false, false);
    push_i2c_word(&mut words, true, false);
    push_i2c_word(&mut words, true, true);
    packed_i2c(words)
}

pub fn i2c_repeated_start_samples() -> PackedSamples {
    let mut words = vec![0b11; 4];
    push_i2c_word(&mut words, true, false);
    push_i2c_word(&mut words, false, false);
    for bit in (0..8).rev() {
        push_i2c_bit(&mut words, (((0x50 << 1) >> bit) & 1) != 0);
    }
    push_i2c_bit(&mut words, false);
    push_i2c_word(&mut words, false, true);
    push_i2c_word(&mut words, true, true);
    push_i2c_word(&mut words, true, false);
    push_i2c_word(&mut words, false, false);
    for bit in (0..8).rev() {
        push_i2c_bit(&mut words, ((((0x50 << 1) | 1) >> bit) & 1) != 0);
    }
    push_i2c_bit(&mut words, false);
    push_i2c_word(&mut words, false, false);
    push_i2c_word(&mut words, true, false);
    push_i2c_word(&mut words, true, true);
    packed_i2c(words)
}

pub fn i2c_simultaneous_scl_sda_changes() -> PackedSamples {
    packed_i2c(vec![0b11, 0b11, 0b00, 0b00, 0b11, 0b11])
}

pub fn spi_samples(
    mode: u8,
    bit_order: BitOrder,
    bits_per_word: u8,
    mosi_word: u16,
    miso_word: u16,
    active_high_cs: bool,
) -> PackedSamples {
    let cpol = mode >= 2;
    let cpha = mode % 2 == 1;
    let selected_cs = active_high_cs;
    let idle_cs = !active_high_cs;
    let mut words = vec![spi_word(cpol, false, false, idle_cs); 2];
    words.push(spi_word(cpol, false, false, selected_cs));
    let mut clk = cpol;
    for bit_index in 0..bits_per_word {
        let mosi_level = spi_fixture_bit(mosi_word, bit_index, bits_per_word, bit_order);
        let miso_level = spi_fixture_bit(miso_word, bit_index, bits_per_word, bit_order);
        if cpha {
            clk = !clk;
            words.push(spi_word(clk, mosi_level, miso_level, selected_cs));
            clk = !clk;
            words.push(spi_word(clk, mosi_level, miso_level, selected_cs));
        } else {
            words.push(spi_word(clk, mosi_level, miso_level, selected_cs));
            clk = !clk;
            words.push(spi_word(clk, mosi_level, miso_level, selected_cs));
            clk = !clk;
            words.push(spi_word(clk, mosi_level, miso_level, selected_cs));
        }
    }
    words.push(spi_word(cpol, false, false, idle_cs));
    packed_spi(words)
}

pub fn spi_cs_deasserts_mid_word() -> PackedSamples {
    let mut samples = spi_samples(0, BitOrder::MsbFirst, 8, 0xF0, 0, false);
    for word in samples.words.iter_mut().skip(9) {
        *word |= 1 << 3;
    }
    samples
}

fn push_i2c_bit(words: &mut Vec<u16>, sda: bool) {
    push_i2c_word(words, false, sda);
    push_i2c_word(words, true, sda);
    push_i2c_word(words, false, sda);
}

fn push_i2c_word(words: &mut Vec<u16>, scl: bool, sda: bool) {
    let word = u16::from(scl) | (u16::from(sda) << 1);
    words.resize(words.len() + 2, word);
}

fn packed_i2c(words: Vec<u16>) -> PackedSamples {
    PackedSamples {
        sample_rate_hz: 100_000,
        sample_period_ps: 10_000_000,
        sample_count: words.len(),
        channels: vec![
            ChannelMapping {
                name: "scl".to_string(),
                bit: 0,
            },
            ChannelMapping {
                name: "sda".to_string(),
                bit: 1,
            },
        ],
        words,
    }
}

fn packed_spi(words: Vec<u16>) -> PackedSamples {
    PackedSamples {
        sample_rate_hz: 1_000_000,
        sample_period_ps: 1_000_000,
        sample_count: words.len(),
        channels: vec![
            ChannelMapping {
                name: "sclk".to_string(),
                bit: 0,
            },
            ChannelMapping {
                name: "mosi".to_string(),
                bit: 1,
            },
            ChannelMapping {
                name: "miso".to_string(),
                bit: 2,
            },
            ChannelMapping {
                name: "cs".to_string(),
                bit: 3,
            },
        ],
        words,
    }
}

fn spi_fixture_bit(word: u16, bit_index: u8, bits_per_word: u8, bit_order: BitOrder) -> bool {
    let shift = match bit_order {
        BitOrder::MsbFirst => bits_per_word - 1 - bit_index,
        BitOrder::LsbFirst => bit_index,
    };
    ((word >> shift) & 1) != 0
}

fn spi_word(sclk: bool, mosi: bool, miso: bool, cs: bool) -> u16 {
    u16::from(sclk) | (u16::from(mosi) << 1) | (u16::from(miso) << 2) | (u16::from(cs) << 3)
}
