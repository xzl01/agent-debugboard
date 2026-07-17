// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::schema::DecodeError;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct PackedSamples {
    pub sample_rate_hz: u64,
    pub sample_period_ps: u64,
    pub sample_count: usize,
    pub channels: Vec<ChannelMapping>,
    pub words: Vec<u16>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct ChannelMapping {
    pub name: String,
    pub bit: u8,
}

pub fn validate_samples(samples: &PackedSamples) -> Result<(), DecodeError> {
    if samples.sample_rate_hz == 0 || samples.sample_period_ps == 0 {
        return Err(DecodeError::InvalidSamples(
            "sample rate and period must be positive".to_string(),
        ));
    }
    if samples.sample_count == 0 {
        return Err(DecodeError::InvalidSamples(
            "sample count must be positive".to_string(),
        ));
    }
    if samples.words.len() < samples.sample_count {
        return Err(DecodeError::InvalidSamples(
            "packed word count is shorter than sample count".to_string(),
        ));
    }
    let mut seen = BTreeMap::new();
    for channel in &samples.channels {
        if channel.bit > 15 {
            return Err(DecodeError::InvalidSamples(format!(
                "channel {} uses bit {} outside u16 sample word",
                channel.name, channel.bit
            )));
        }
        if seen.insert(channel.name.clone(), channel.bit).is_some() {
            return Err(DecodeError::InvalidSamples(format!(
                "duplicate channel name {}",
                channel.name
            )));
        }
    }
    Ok(())
}

pub(crate) fn channel_bit(samples: &PackedSamples, name: &str) -> Result<u8, DecodeError> {
    samples
        .channels
        .iter()
        .find(|channel| channel.name == name)
        .map(|channel| channel.bit)
        .ok_or_else(|| DecodeError::InvalidOptions(format!("unknown channel {name}")))
}

pub(crate) fn bit_at(samples: &PackedSamples, sample: usize, bit: u8) -> bool {
    ((samples.words[sample] >> bit) & 1) != 0
}
