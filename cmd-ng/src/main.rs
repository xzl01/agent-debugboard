// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

mod adc;
mod app;
mod cli;
mod client;
mod client_url;
mod config_command;
mod desktop;
mod discovery;
mod discovery_command;
mod json_contract;
mod monitoring;
mod net_command;
mod persistent_config;
mod persistent_config_render;
mod persistent_config_validate;
mod persistent_config_value;
mod recorder;
mod task_api;
mod task_blob;
mod task_catalog;
mod task_command;
mod task_envelope;
mod task_error;
mod task_execution;
mod task_output;
mod task_parse;
mod task_store;
mod test;
mod test_assertions;
mod test_report;
mod test_runner;
mod test_script;
mod test_serial;
mod tui;
mod ws_client;
mod ws_status;

#[cfg(test)]
mod client_tests;
#[cfg(test)]
mod discovery_test_support;
#[cfg(test)]
mod persistent_config_model_tests;
#[cfg(test)]
mod persistent_config_tests;
#[cfg(test)]
mod task_blob_tests;
#[cfg(test)]
mod task_cancellation_tests;
#[cfg(test)]
mod task_catalog_execution_tests;
#[cfg(test)]
mod task_catalog_tests;
#[cfg(test)]
mod task_cleanup_tests;
#[cfg(test)]
mod task_command_tests;
#[cfg(test)]
mod task_stored_execution_tests;
#[cfg(test)]
mod task_test_support;
#[cfg(test)]
mod task_transport_tests;
#[cfg(test)]
mod task_v2_tests;

use std::ffi::OsString;
use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<OsString> = std::env::args_os().collect();
    match app::run(args) {
        Ok(code) => ExitCode::from(code),
        Err(err) => {
            eprintln!("{err}");
            ExitCode::from(1)
        }
    }
}
