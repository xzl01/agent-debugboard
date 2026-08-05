use crate::client::BoardTransport;
use crate::persistent_config::{ConfigItemId, PersistentConfigResponse};
use crate::persistent_config_render::{error_text, write_clear, write_save, write_show};
use anyhow::Result;
use std::io::Write;

pub(crate) enum ConfigCommand {
    Show,
    Save {
        items: Vec<ConfigItemId>,
        confirm: bool,
    },
    Clear,
}

pub(crate) fn parse(args: &[String]) -> Result<ConfigCommand, String> {
    let cleaned: Vec<&String> = args
        .iter()
        .filter(|arg| {
            arg.as_str() != "--json" && arg.as_str() != "-v" && arg.as_str() != "--verbose"
        })
        .collect();
    if cleaned.first().map(|arg| arg.as_str()) != Some("config") {
        return Err("usage: radxa-linkr-debuggerctl config show|save|clear".to_string());
    }
    match cleaned.get(1).map(|arg| arg.as_str()) {
        Some("show") if cleaned.len() == 2 => Ok(ConfigCommand::Show),
        Some("save") => {
            let confirm = cleaned.iter().any(|arg| arg.as_str() == "--confirm");
            let items = cleaned[2..]
                .iter()
                .filter(|arg| arg.as_str() != "--confirm")
                .map(|arg| ConfigItemId((*arg).clone()))
                .collect::<Vec<_>>();
            if items.is_empty() {
                return Err(
                    "usage: radxa-linkr-debuggerctl config save [--confirm] <item-id>..."
                        .to_string(),
                );
            }
            Ok(ConfigCommand::Save { items, confirm })
        }
        Some("clear") if cleaned.len() == 2 => Ok(ConfigCommand::Clear),
        _ => Err("usage: radxa-linkr-debuggerctl config show|save|clear".to_string()),
    }
}

pub(crate) fn run<TClient>(
    client: &TClient,
    args: &[String],
    json_output: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8>
where
    TClient: BoardTransport,
{
    let command = match parse(args) {
        Ok(command) => command,
        Err(message) => return write_usage_error(json_output, stdout, stderr, &message),
    };
    let response = match &command {
        ConfigCommand::Show => client.config_show(),
        ConfigCommand::Save { items, confirm } => client.config_save(items, *confirm),
        ConfigCommand::Clear => client.config_clear(),
    };
    match response {
        Ok(response) => write_response(&command, response, json_output, stdout, stderr),
        Err(error) => write_transport_error(json_output, stdout, stderr, &error.to_string()),
    }
}

fn write_usage_error(
    json_output: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
    message: &str,
) -> Result<u8> {
    if json_output {
        writeln!(stdout, "{{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":false,\"command\":\"config\",\"error\":{{\"code\":\"usage\",\"message\":{}}}}}", serde_json::to_string(message)?)?;
    } else {
        writeln!(stderr, "{message}")?;
    }
    Ok(2)
}

fn write_transport_error(
    json_output: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
    message: &str,
) -> Result<u8> {
    if json_output {
        writeln!(stdout, "{{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":false,\"command\":\"config\",\"error\":{{\"code\":\"transport_error\",\"message\":{}}}}}", serde_json::to_string(message)?)?;
    } else {
        writeln!(stderr, "{message}")?;
    }
    Ok(1)
}

fn write_response(
    command: &ConfigCommand,
    response: PersistentConfigResponse,
    json_output: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8> {
    if json_output {
        write!(stdout, "{}", response.raw_json)?;
        if !response.raw_json.ends_with('\n') {
            writeln!(stdout)?;
        }
        return Ok(if response.envelope.ok { 0 } else { 1 });
    }
    if !response.envelope.ok {
        writeln!(stderr, "{}", error_text(&response.envelope))?;
        return Ok(1);
    }
    match command {
        ConfigCommand::Show => write_show(&response.envelope, stdout)?,
        ConfigCommand::Save { .. } => write_save(&response.envelope, stdout)?,
        ConfigCommand::Clear => write_clear(&response.envelope, stdout)?,
    }
    Ok(0)
}
