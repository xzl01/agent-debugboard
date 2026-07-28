// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

mod support;

use radxa_logic_decoder::{
    decode, decode_json, BitOrder, ChannelMapping, DecodeError, DecodeRequest, I2cOptions,
    PackedSamples, Parity, ProtocolRequest, SpiOptions, UartOptions, RESULT_SCHEMA_VERSION,
    SCHEMA_VERSION,
};
use serde_json::{json, Value};
use support::{
    i2c_repeated_start_samples, i2c_samples, i2c_simultaneous_scl_sda_changes,
    spi_cs_deasserts_mid_word, spi_samples, uart_samples,
};

#[test]
fn uart_decodes_8n1_byte() {
    let request = uart_request(uart_samples(0x55, false, None, true), Parity::None);

    let result = decode(&request).expect("UART decode should succeed");

    assert!(result.diagnostics.is_empty());
    assert_eq!(result.schema_version, RESULT_SCHEMA_VERSION);
    assert_eq!(result.annotations.len(), 1);
    assert_eq!(result.annotations[0].class, "data");
    assert_eq!(result.annotations[0].data["value"], json!(0x55));
    assert_eq!(result.annotations[0].short_text, "0x55 'U'");
}

#[test]
fn uart_shows_ascii_for_printable_and_hex_for_control() {
    let request_printable = uart_request(uart_samples(0x31, false, None, true), Parity::None);
    let result_printable = decode(&request_printable).expect("decode should succeed");
    assert_eq!(result_printable.annotations[0].short_text, "0x31 '1'");

    let request_control = uart_request(uart_samples(0x0D, false, None, true), Parity::None);
    let result_control = decode(&request_control).expect("decode should succeed");
    assert_eq!(result_control.annotations[0].short_text, "0x0D");
}

#[test]
fn uart_decodes_consecutive_frames_without_drift() {
    let bit_samples = 10usize;
    let values: &[u16] = &[0x6C, 0x6F, 0x6C, 0x21];
    let mut logical_bits = Vec::new();
    for &val in values {
        logical_bits.push(true);
        logical_bits.push(true);
        logical_bits.push(false);
        for bit in 0..8 {
            logical_bits.push(((val >> bit) & 1) != 0);
        }
        logical_bits.push(true);
    }
    logical_bits.extend([true, true]);

    let mut words = Vec::new();
    for level in logical_bits {
        words.resize(words.len() + bit_samples, u16::from(level));
    }

    let request = DecodeRequest {
        schema_version: SCHEMA_VERSION.to_string(),
        samples: PackedSamples {
            sample_rate_hz: 10_000,
            sample_period_ps: 100_000_000,
            sample_count: words.len(),
            channels: vec![ChannelMapping {
                name: "rx".to_string(),
                bit: 0,
            }],
            words,
        },
        protocol: ProtocolRequest::Uart(UartOptions {
            rx: "rx".to_string(),
            baud: 1_000,
            data_bits: 8,
            parity: Parity::None,
            stop_bits: 1,
            inverted: false,
        }),
    };

    let result = decode(&request).expect("multi-frame decode should succeed");
    assert_eq!(result.annotations.len(), 4);
    assert_eq!(result.annotations[0].data["value"], json!(0x6C));
    assert_eq!(result.annotations[1].data["value"], json!(0x6F));
    assert_eq!(result.annotations[2].data["value"], json!(0x6C));
    assert_eq!(result.annotations[3].data["value"], json!(0x21));
}

#[test]
fn uart_reports_parity_and_stop_errors() {
    let request = uart_request(uart_samples(0x33, false, Some(true), false), Parity::Even);

    let result = decode(&request).expect("UART decode should succeed with diagnostics");

    assert_eq!(result.annotations[0].data["value"], json!(0x33));
    assert!(result.diagnostics.iter().any(|d| d.code == "uart-parity"));
    assert!(result.diagnostics.iter().any(|d| d.code == "uart-stop-bit"));
}

#[test]
fn uart_reports_truncated_frame() {
    let mut samples = uart_samples(0xA5, false, None, true);
    samples.sample_count = 35;
    samples.words.truncate(35);
    let request = uart_request(samples, Parity::None);

    let result = decode(&request).expect("truncated UART capture should be diagnosable");

    assert!(result.annotations.is_empty());
    assert_eq!(result.diagnostics[0].code, "truncated-uart-frame");
}

#[test]
fn uart_decodes_exact_boundary_frame_completion() {
    let mut samples = uart_samples(0xA5, false, None, true);
    samples.sample_count = 120;
    samples.words.truncate(120);
    let request = uart_request(samples, Parity::None);

    let result = decode(&request).expect("exact-boundary UART frame should decode");

    assert!(result.diagnostics.is_empty());
    assert_eq!(result.annotations[0].data["value"], json!(0xA5));
    assert_eq!(result.annotations[0].end_sample, 120);
}

#[test]
fn uart_decodes_inverted_signal() {
    let request = DecodeRequest {
        schema_version: SCHEMA_VERSION.to_string(),
        samples: uart_samples(0xC3, true, None, true),
        protocol: ProtocolRequest::Uart(UartOptions {
            rx: "rx".to_string(),
            baud: 1_000,
            data_bits: 8,
            parity: Parity::None,
            stop_bits: 1,
            inverted: true,
        }),
    };

    let result = decode(&request).expect("inverted UART should decode");

    assert_eq!(result.annotations[0].data["value"], json!(0xC3));
}

#[test]
fn uart_decodes_frame_starting_at_start_bit() {
    let bit_samples = 10usize;
    let value: u16 = 0x31;
    let mut logical_bits = Vec::new();
    logical_bits.push(false);
    for bit in 0..8 {
        logical_bits.push(((value >> bit) & 1) != 0);
    }
    logical_bits.push(true);
    logical_bits.extend([true, true]);

    let mut words = Vec::new();
    for level in logical_bits {
        words.resize(words.len() + bit_samples, u16::from(level));
    }

    let request = DecodeRequest {
        schema_version: SCHEMA_VERSION.to_string(),
        samples: PackedSamples {
            sample_rate_hz: 10_000,
            sample_period_ps: 100_000_000,
            sample_count: words.len(),
            channels: vec![ChannelMapping {
                name: "rx".to_string(),
                bit: 0,
            }],
            words,
        },
        protocol: ProtocolRequest::Uart(UartOptions {
            rx: "rx".to_string(),
            baud: 1_000,
            data_bits: 8,
            parity: Parity::None,
            stop_bits: 1,
            inverted: false,
        }),
    };

    let result = decode(&request).expect("frame starting at start bit should decode");
    assert_eq!(result.annotations[0].data["value"], json!(0x31));
    assert!(result.diagnostics.is_empty());
}

#[test]
fn uart_decodes_press_waveform_from_sample_zero_without_rescanning_edges() {
    let bit_samples = 10usize;
    let mut words = Vec::new();
    for &value in &[0x50u16, 0x72, 0x65, 0x73, 0x73] {
        words.resize(words.len() + bit_samples, 0);
        for bit in 0..8 {
            words.resize(
                words.len() + bit_samples,
                u16::from(((value >> bit) & 1) != 0),
            );
        }
        words.resize(words.len() + bit_samples, 1);
    }

    let request = DecodeRequest {
        schema_version: SCHEMA_VERSION.to_string(),
        samples: PackedSamples {
            sample_rate_hz: 10_000,
            sample_period_ps: 100_000_000,
            sample_count: words.len(),
            channels: vec![ChannelMapping {
                name: "rx".to_string(),
                bit: 0,
            }],
            words,
        },
        protocol: ProtocolRequest::Uart(UartOptions {
            rx: "rx".to_string(),
            baud: 1_000,
            data_bits: 8,
            parity: Parity::None,
            stop_bits: 1,
            inverted: false,
        }),
    };

    let result = decode(&request).expect("Press waveform should decode");
    let annotations: Vec<(usize, usize, u16)> = result
        .annotations
        .iter()
        .map(|annotation| {
            (
                annotation.start_sample,
                annotation.end_sample,
                annotation.data["value"]
                    .as_u64()
                    .expect("UART value should be numeric") as u16,
            )
        })
        .collect();

    assert_eq!(
        annotations,
        vec![
            (0, 100, 0x50),
            (100, 200, 0x72),
            (200, 300, 0x65),
            (300, 400, 0x73),
            (400, 500, 0x73),
        ]
    );
    assert!(result.diagnostics.is_empty());
}

#[test]
fn i2c_decodes_start_address_data_nack_stop() {
    let request = i2c_request(i2c_samples(&[(0x50 << 1, true), (0xA5, false)]));

    let result = decode(&request).expect("I2C decode should succeed");

    let classes: Vec<&str> = result
        .annotations
        .iter()
        .map(|a| a.class.as_str())
        .collect();
    assert_eq!(classes, vec!["start", "address", "data", "stop"]);
    assert_eq!(result.annotations[1].data["address"], json!(0x50));
    assert_eq!(result.annotations[1].data["read"], json!(false));
    assert_eq!(result.annotations[1].data["ack"], json!(true));
    assert_eq!(result.annotations[2].data["value"], json!(0xA5));
    assert_eq!(result.annotations[2].data["ack"], json!(false));
}

#[test]
fn i2c_decodes_repeated_start() {
    let request = i2c_request(i2c_repeated_start_samples());

    let result = decode(&request).expect("I2C repeated START should decode");

    assert!(result.annotations.iter().any(|a| a.class == "restart"));
    assert!(result
        .annotations
        .iter()
        .any(|a| a.data["read"] == json!(true)));
}

#[test]
fn i2c_reports_truncated_byte() {
    let mut samples = i2c_samples(&[(0xA0, true)]);
    samples.sample_count = 24;
    samples.words.truncate(samples.sample_count);
    let request = i2c_request(samples);

    let result = decode(&request).expect("truncated I2C should be diagnosable");

    assert!(result
        .diagnostics
        .iter()
        .any(|d| d.code == "truncated-i2c-byte"));
}

#[test]
fn i2c_does_not_emit_start_or_stop_when_scl_and_sda_change_together() {
    let request = i2c_request(i2c_simultaneous_scl_sda_changes());

    let result = decode(&request).expect("simultaneous I2C changes should decode cleanly");

    assert!(result.annotations.is_empty());
    assert!(result.diagnostics.is_empty());
}

#[test]
fn spi_decodes_mode0_msb_first_duplex_word() {
    let request = spi_request(
        spi_samples(0, BitOrder::MsbFirst, 8, 0xA5, 0x3C, false),
        0,
        BitOrder::MsbFirst,
        false,
    );

    let result = decode(&request).expect("SPI mode 0 should decode");

    assert!(result.diagnostics.is_empty());
    assert_eq!(result.annotations[0].data["mosi"], json!(0xA5));
    assert_eq!(result.annotations[0].data["miso"], json!(0x3C));
}

#[test]
fn spi_decodes_mode3_lsb_first_active_high_cs() {
    let request = DecodeRequest {
        schema_version: SCHEMA_VERSION.to_string(),
        samples: spi_samples(3, BitOrder::LsbFirst, 8, 0x96, 0, true),
        protocol: ProtocolRequest::Spi(SpiOptions {
            sclk: "sclk".to_string(),
            mosi: Some("mosi".to_string()),
            miso: None,
            cs: Some("cs".to_string()),
            cs_active_high: true,
            mode: 3,
            bit_order: BitOrder::LsbFirst,
            bits_per_word: 8,
        }),
    };

    let result = decode(&request).expect("SPI mode 3 should decode");

    assert_eq!(result.annotations[0].data["mosi"], json!(0x96));
}

#[test]
fn spi_reports_capture_end_truncated_word() {
    let mut samples = spi_samples(0, BitOrder::MsbFirst, 8, 0xF0, 0, false);
    samples.sample_count -= 6;
    samples.words.truncate(samples.sample_count);
    let request = spi_request(samples, 0, BitOrder::MsbFirst, false);

    let result = decode(&request).expect("truncated SPI should be diagnosable");

    assert!(result
        .diagnostics
        .iter()
        .any(|d| d.code == "truncated-spi-word"));
}

#[test]
fn spi_reports_cs_deassertion_truncated_word_without_data_annotation() {
    let request = spi_request(spi_cs_deasserts_mid_word(), 0, BitOrder::MsbFirst, false);

    let result = decode(&request).expect("CS-truncated SPI should be diagnosable");

    assert!(result.annotations.is_empty());
    assert!(result.diagnostics.iter().any(|d| {
        d.code == "truncated-spi-word"
            && d.message == "chip select deasserted before a complete SPI word was available"
    }));
}

#[test]
fn checked_in_uart_fixture_decodes() {
    let request: DecodeRequest = serde_json::from_str(include_str!(
        "../fixtures/self-generated/uart_0x55_8n1_request.json"
    ))
    .expect("fixture should parse");

    let result = decode(&request).expect("checked-in fixture should decode");

    assert_eq!(result.annotations[0].data["value"], json!(0x55));
}

#[test]
fn invalid_options_are_errors() {
    let request = DecodeRequest {
        schema_version: SCHEMA_VERSION.to_string(),
        samples: uart_samples(0, false, None, true),
        protocol: ProtocolRequest::Uart(UartOptions {
            rx: "missing".to_string(),
            baud: 0,
            data_bits: 8,
            parity: Parity::None,
            stop_bits: 1,
            inverted: false,
        }),
    };

    assert!(matches!(
        decode(&request),
        Err(DecodeError::InvalidOptions(_))
    ));
}

#[test]
fn json_boundary_reports_success_and_errors() {
    let request = uart_request(uart_samples(0x42, false, None, true), Parity::None);
    let ok: Value = serde_json::from_str(&decode_json(&serde_json::to_string(&request).unwrap()))
        .expect("JSON response should parse");
    let err: Value =
        serde_json::from_str(&decode_json("{")).expect("JSON error response should parse");

    assert_eq!(ok["ok"], json!(true));
    assert_eq!(err["ok"], json!(false));
}

fn uart_request(samples: radxa_logic_decoder::PackedSamples, parity: Parity) -> DecodeRequest {
    DecodeRequest {
        schema_version: SCHEMA_VERSION.to_string(),
        samples,
        protocol: ProtocolRequest::Uart(UartOptions {
            rx: "rx".to_string(),
            baud: 1_000,
            data_bits: 8,
            parity,
            stop_bits: 1,
            inverted: false,
        }),
    }
}

fn i2c_request(samples: radxa_logic_decoder::PackedSamples) -> DecodeRequest {
    DecodeRequest {
        schema_version: SCHEMA_VERSION.to_string(),
        samples,
        protocol: ProtocolRequest::I2c(I2cOptions {
            scl: "scl".to_string(),
            sda: "sda".to_string(),
        }),
    }
}

fn spi_request(
    samples: radxa_logic_decoder::PackedSamples,
    mode: u8,
    bit_order: BitOrder,
    cs_active_high: bool,
) -> DecodeRequest {
    DecodeRequest {
        schema_version: SCHEMA_VERSION.to_string(),
        samples,
        protocol: ProtocolRequest::Spi(SpiOptions {
            sclk: "sclk".to_string(),
            mosi: Some("mosi".to_string()),
            miso: Some("miso".to_string()),
            cs: Some("cs".to_string()),
            cs_active_high,
            mode,
            bit_order,
            bits_per_word: 8,
        }),
    }
}

#[test]
fn uart_1mhz_115200_decodes_ascii_1() {
    // 1MHz / 115200 = 8.68 samples per bit
    // Simulate '1' (0x31) with start=0, data=1,0,0,0,1,1,0,0, stop=1
    let bit_samples_f64: f64 = 1_000_000.0 / 115_200.0; // 8.680555...
    let mut words = vec![1u16; 20]; // idle high before start

    // Start bit (low) at samples_per_bit
    words.resize(words.len() + bit_samples_f64.ceil() as usize, 0);

    // Data bits: 1,0,0,0,1,1,0,0 (LSB first)
    let data_bits = [1u16, 0, 0, 0, 1, 1, 0, 0];
    for &bit in &data_bits {
        words.resize(words.len() + bit_samples_f64.ceil() as usize, bit);
    }

    // Stop bit (high)
    words.resize(words.len() + bit_samples_f64.ceil() as usize, 1);

    let samples = radxa_logic_decoder::PackedSamples {
        sample_rate_hz: 1_000_000,
        sample_period_ps: 1_000_000,
        sample_count: words.len(),
        channels: vec![radxa_logic_decoder::ChannelMapping {
            name: "rx".to_string(),
            bit: 0,
        }],
        words,
    };

    let request = DecodeRequest {
        schema_version: SCHEMA_VERSION.to_string(),
        samples,
        protocol: ProtocolRequest::Uart(UartOptions {
            rx: "rx".to_string(),
            baud: 115200,
            data_bits: 8,
            parity: Parity::None,
            stop_bits: 1,
            inverted: false,
        }),
    };

    let result = decode(&request).expect("1MHz/115200 decode should succeed");
    assert_eq!(result.annotations.len(), 1);
    assert_eq!(result.annotations[0].data["value"], json!(0x31));
}
