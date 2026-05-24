use clap::Parser;

#[derive(Debug, Parser)]
#[command(
    name = "agent-debugboardctl",
    disable_version_flag = true,
    about = "Rust host CLI/TUI for Agent DebugBoard"
)]
pub struct Cli {
    #[arg(long = "url")]
    pub url: Option<String>,
    #[arg(long = "addr")]
    pub addr: Option<String>,
    #[arg(long = "port")]
    pub port: Option<String>,
    #[arg(long = "timeout", default_value = "2s", value_parser = parse_timeout)]
    pub timeout: std::time::Duration,
    #[arg(long = "raw")]
    pub raw: bool,
    #[arg(long = "json")]
    pub json: bool,
    #[arg(short = 'v', long = "verbose")]
    pub verbose: bool,
    #[arg(long = "version")]
    pub version: bool,
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    pub command_args: Vec<String>,
}

fn parse_timeout(value: &str) -> Result<std::time::Duration, String> {
    if let Ok(duration) = humantime::parse_duration(value) {
        if duration.is_zero() {
            return Err("timeout must be greater than zero".to_string());
        }
        return Ok(duration);
    }

    let seconds: f64 = value
        .parse()
        .map_err(|_| "timeout must be a duration like 2s or seconds like 0.5".to_string())?;
    if seconds <= 0.0 {
        return Err("timeout must be greater than zero".to_string());
    }

    Ok(std::time::Duration::from_secs_f64(seconds))
}

#[cfg(test)]
mod tests {
    use super::{parse_timeout, Cli};
    use clap::Parser;
    use std::time::Duration;

    #[test]
    fn parses_human_duration() {
        assert_eq!(parse_timeout("2s").unwrap(), Duration::from_secs(2));
    }

    #[test]
    fn parses_numeric_seconds() {
        assert_eq!(parse_timeout("0.5").unwrap(), Duration::from_millis(500));
    }

    #[test]
    fn keeps_trailing_args_raw() {
        let cli = Cli::parse_from(["cmd", "adc", "read", "-v", "5v_out"]);
        assert_eq!(cli.command_args, vec!["adc", "read", "-v", "5v_out"]);
    }
}
