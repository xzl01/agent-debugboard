// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

mod adc;
mod app;
mod cli;
mod client;
mod config_command;
mod json_contract;
mod monitoring;
mod persistent_config;
mod persistent_config_render;
mod persistent_config_validate;
mod persistent_config_value;
mod recorder;
mod test;
mod test_assertions;
mod test_report;
mod test_runner;
mod test_script;
mod test_serial;
mod tui;
mod ws_client;

#[cfg(test)]
mod persistent_config_model_tests;
#[cfg(test)]
mod persistent_config_tests;

use std::process::ExitCode;

fn main() -> ExitCode {
    match app::run(std::env::args_os()) {
        Ok(code) => ExitCode::from(code),
        Err(err) => {
            eprintln!("{err}");
            ExitCode::from(1)
        }
    }
}
