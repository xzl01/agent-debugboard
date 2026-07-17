// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

pub mod i2c;
pub mod spi;
pub mod uart;

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "name", content = "options", rename_all = "lowercase")]
pub enum ProtocolRequest {
    Uart(UartOptions),
    I2c(I2cOptions),
    Spi(SpiOptions),
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum Parity {
    None,
    Even,
    Odd,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct UartOptions {
    pub rx: String,
    pub baud: u32,
    pub data_bits: u8,
    pub parity: Parity,
    pub stop_bits: u8,
    pub inverted: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct I2cOptions {
    pub scl: String,
    pub sda: String,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum BitOrder {
    MsbFirst,
    LsbFirst,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct SpiOptions {
    pub sclk: String,
    pub mosi: Option<String>,
    pub miso: Option<String>,
    pub cs: Option<String>,
    pub cs_active_high: bool,
    pub mode: u8,
    pub bit_order: BitOrder,
    pub bits_per_word: u8,
}
