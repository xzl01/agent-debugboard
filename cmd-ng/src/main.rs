mod adc;
mod app;
mod cli;
mod client;
mod json_contract;
mod monitoring;
mod recorder;
mod tui;
mod ws_client;

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
