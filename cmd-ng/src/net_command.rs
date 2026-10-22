// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::client::{BoardHttpError, BoardRequest, BoardTransport};
use crate::json_contract::JSON_SCHEMA;
use anyhow::{bail, Context, Result};
use reqwest::Method;
use serde_json::Value;
use std::io::Write;

const USAGE: &str =
    "usage: radxa-linkr-debuggerctl ip [show | mac <aa:bb:cc:dd:ee:ff> | network <RFC1918 /30 base>] (net is an alias)";

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum NetCommand {
    Show,
    SetMac(String),
    SetNetwork(String),
}

fn parse_mac(value: &str) -> Result<String, String> {
    let parts: Vec<&str> = value.split(':').collect();
    if parts.len() != 6 {
        return Err(USAGE.to_string());
    }
    let mut bytes = [0u8; 6];
    for (index, part) in parts.iter().enumerate() {
        if part.len() != 2 {
            return Err(USAGE.to_string());
        }
        bytes[index] = u8::from_str_radix(part, 16).map_err(|_| USAGE.to_string())?;
    }
    if bytes[0] & 0x03 != 0x02 {
        return Err(USAGE.to_string());
    }
    Ok(format!(
        "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]
    ))
}

fn parse_ipv4(value: &str) -> Option<u32> {
    let parts: Vec<&str> = value.split('.').collect();
    if parts.len() != 4 {
        return None;
    }
    let mut address = 0u32;
    for part in parts {
        if part.is_empty() || part.len() > 3 || !part.bytes().all(|byte| byte.is_ascii_digit()) {
            return None;
        }
        let octet = part.parse::<u32>().ok()?;
        if octet > 255 {
            return None;
        }
        address = (address << 8) | octet;
    }
    Some(address)
}

fn is_private_slash30(address: u32) -> bool {
    address & 0x03 == 0
        && ((address & 0xff00_0000) == 0x0a00_0000
            || (address & 0xfff0_0000) == 0xac10_0000
            || (address & 0xffff_0000) == 0xc0a8_0000)
}

fn parse_network(value: &str) -> Result<String, String> {
    let base = value.strip_suffix("/30").unwrap_or(value);
    let address = parse_ipv4(base).ok_or_else(|| USAGE.to_string())?;
    if !is_private_slash30(address) {
        return Err(USAGE.to_string());
    }
    Ok(format!(
        "{}.{}.{}.{}",
        (address >> 24) & 0xff,
        (address >> 16) & 0xff,
        (address >> 8) & 0xff,
        address & 0xff
    ))
}

pub(crate) fn parse(args: &[String]) -> Result<NetCommand, String> {
    let cleaned: Vec<&String> = args
        .iter()
        .filter(|arg| {
            arg.as_str() != "--json" && arg.as_str() != "-v" && arg.as_str() != "--verbose"
        })
        .collect();

    match cleaned.first().map(|arg| arg.as_str()) {
        Some("ip") | Some("net") => {}
        _ => return Err(USAGE.to_string()),
    }

    match (cleaned.len(), cleaned.get(1).map(|arg| arg.as_str())) {
        (1, _) | (2, Some("show")) => Ok(NetCommand::Show),
        (3, Some("mac")) => Ok(NetCommand::SetMac(parse_mac(cleaned[2])?)),
        (3, Some("network")) => Ok(NetCommand::SetNetwork(parse_network(cleaned[2])?)),
        _ => Err(USAGE.to_string()),
    }
}

struct NetStatus {
    mac: String,
    origin: String,
    network: String,
    ip: String,
    dhcp_base: String,
    url: String,
}

fn parse_status(raw: &str, expected_action: &str) -> Result<NetStatus> {
    let value: Value = serde_json::from_str(raw).context("decode net response")?;
    if value.get("schema").and_then(Value::as_str) != Some(JSON_SCHEMA)
        || value.get("command").and_then(Value::as_str) != Some("net")
    {
        bail!("response is not a net envelope");
    }
    if value.get("ok").and_then(Value::as_bool) != Some(true) {
        let message = value
            .pointer("/error/message")
            .and_then(Value::as_str)
            .unwrap_or("net request failed");
        bail!("{message}");
    }
    if value.get("action").and_then(Value::as_str) != Some(expected_action) {
        bail!("unexpected net action");
    }

    let mac = value
        .get("mac")
        .and_then(Value::as_str)
        .filter(|mac| parse_mac(mac).is_ok())
        .ok_or_else(|| anyhow::anyhow!("invalid mac in net response"))?
        .to_ascii_lowercase();
    let origin = value
        .get("origin")
        .and_then(Value::as_str)
        .filter(|origin| matches!(*origin, "derived" | "manual"))
        .ok_or_else(|| anyhow::anyhow!("invalid origin in net response"))?;
    let network = value
        .get("network")
        .and_then(Value::as_str)
        .filter(|network| parse_network(network).is_ok())
        .ok_or_else(|| anyhow::anyhow!("invalid network in net response"))?;
    let ip = value
        .get("ip")
        .and_then(Value::as_str)
        .filter(|ip| parse_ipv4(ip).is_some())
        .ok_or_else(|| anyhow::anyhow!("invalid ip in net response"))?;
    let dhcp_base = value
        .get("dhcp_base")
        .and_then(Value::as_str)
        .filter(|ip| parse_ipv4(ip).is_some())
        .ok_or_else(|| anyhow::anyhow!("invalid dhcp_base in net response"))?;
    let url = value
        .get("url")
        .and_then(Value::as_str)
        .filter(|url| url.starts_with("http://") && url.ends_with('/'))
        .ok_or_else(|| anyhow::anyhow!("invalid url in net response"))?;

    Ok(NetStatus {
        mac,
        origin: origin.to_string(),
        network: network.to_string(),
        ip: ip.to_string(),
        dhcp_base: dhcp_base.to_string(),
        url: url.to_string(),
    })
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

    let (request, expected_action) = match command {
        NetCommand::Show => (
            BoardRequest {
                method: Method::GET,
                path: "/api/v1/net".to_string(),
                query: vec![],
                body: None,
            },
            "get",
        ),
        NetCommand::SetMac(mac) => (
            BoardRequest {
                method: Method::PUT,
                path: "/api/v1/net".to_string(),
                query: vec![],
                body: Some(serde_json::json!({ "mac": mac })),
            },
            "set",
        ),
        NetCommand::SetNetwork(network) => (
            BoardRequest {
                method: Method::PUT,
                path: "/api/v1/net".to_string(),
                query: vec![],
                body: Some(serde_json::json!({ "network": network })),
            },
            "set",
        ),
    };

    let raw = match client.send_text(request) {
        Ok(raw) => raw,
        Err(error) => {
            if let Some(error) = error.downcast_ref::<BoardHttpError>() {
                return write_http_error(json_output, stdout, stderr, error);
            }
            return write_transport_error(json_output, stdout, stderr, &error.to_string());
        }
    };

    if json_output {
        write!(stdout, "{raw}")?;
        if !raw.ends_with('\n') {
            writeln!(stdout)?;
        }
    }

    let status = match parse_status(&raw, expected_action) {
        Ok(status) => status,
        Err(error) => {
            if !json_output {
                writeln!(stderr, "{error}")?;
            }
            return Ok(1);
        }
    };

    if !json_output {
        writeln!(
            stdout,
            "mac={} origin={} network={}/30 ip={} dhcp_base={} url={}",
            status.mac, status.origin, status.network, status.ip, status.dhcp_base, status.url
        )?;
    }

    Ok(0)
}

fn write_http_error(
    json_output: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
    error: &BoardHttpError,
) -> Result<u8> {
    if json_output {
        write!(stdout, "{}", error.body)?;
    } else {
        let value = serde_json::from_str::<Value>(&error.body).ok();
        let message = value
            .as_ref()
            .and_then(|value| value.pointer("/error/message"))
            .and_then(Value::as_str);
        let code = value
            .as_ref()
            .and_then(|value| value.pointer("/error/code"))
            .and_then(Value::as_str);
        match (message, code) {
            (Some(message), Some(code)) => writeln!(stderr, "{message} ({code})")?,
            (Some(message), None) => writeln!(stderr, "{message}")?,
            (None, Some(code)) => writeln!(stderr, "{code}")?,
            (None, None) => writeln!(stderr, "{error}")?,
        }
    }
    Ok(1)
}

fn write_usage_error(
    json_output: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
    message: &str,
) -> Result<u8> {
    if json_output {
        writeln!(
            stdout,
            "{{\"schema\":\"{JSON_SCHEMA}\",\"ok\":false,\"command\":\"net\",\"error\":{{\"code\":\"usage\",\"message\":{}}}}}",
            serde_json::to_string(message)?
        )?;
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
        writeln!(
            stdout,
            "{{\"schema\":\"{JSON_SCHEMA}\",\"ok\":false,\"command\":\"net\",\"error\":{{\"code\":\"transport_error\",\"message\":{}}}}}",
            serde_json::to_string(message)?
        )?;
    } else {
        writeln!(stderr, "{message}")?;
    }
    Ok(1)
}

#[cfg(test)]
mod tests {
    use super::{parse, run, NetCommand, USAGE};
    use crate::client::{
        BoardBinaryUpload, BoardClient, BoardHttpError, BoardRequest, BoardTransport,
    };
    use crate::client_tests::mock_http_response;
    use anyhow::Result;
    use reqwest::{Method, StatusCode};
    use serde_json::json;
    use std::cell::RefCell;
    use std::time::Duration;

    fn args(values: &[&str]) -> Vec<String> {
        values.iter().map(|value| value.to_string()).collect()
    }

    #[derive(Default)]
    struct FakeTransport {
        requests: RefCell<Vec<BoardRequest>>,
        response: String,
        error: Option<String>,
    }

    impl BoardTransport for FakeTransport {
        fn send_text(&self, request: BoardRequest) -> Result<String> {
            self.requests.borrow_mut().push(request);
            if let Some(error) = &self.error {
                return Err(anyhow::anyhow!(error.clone()));
            }
            Ok(self.response.clone())
        }

        fn upload_binary(&self, _request: BoardBinaryUpload) -> Result<String> {
            unreachable!()
        }

        fn base_url(&self) -> &str {
            "http://10.0.0.1"
        }
    }

    const MAC: &str = "02:11:22:33:44:55";

    fn envelope(action: &str, network: &str, ip: &str, dhcp_base: &str) -> String {
        format!(
            "{{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":true,\"command\":\"net\",\"action\":\"{action}\",\"mac\":\"{MAC}\",\"origin\":\"{}\",\"network\":\"{network}\",\"prefix\":30,\"ip\":\"{ip}\",\"dhcp_base\":\"{dhcp_base}\",\"url\":\"http://{ip}/\",\"reconnect_required\":{}}}\n",
            if action == "set" { "manual" } else { "derived" },
            action == "set"
        )
    }

    fn http_failure_cases() -> [(StatusCode, &'static str, &'static str); 2] {
        [
            (
                StatusCode::BAD_REQUEST,
                "invalid_identity",
                "Identity rejected",
            ),
            (
                StatusCode::INTERNAL_SERVER_ERROR,
                "set_failed",
                "Could not save identity",
            ),
        ]
    }

    fn error_envelope(code: &str, message: &str) -> String {
        format!(
            " {{ \"schema\":\"radxa-linkr-debugger.v1\",\"ok\":false,\"command\":\"net\",\"error\":{{\"code\":\"{code}\",\"message\":\"{message}\"}} }} \n"
        )
    }

    #[test]
    fn parses_show_and_identity_updates_for_ip_and_net_aliases() {
        assert_eq!(parse(&args(&["ip"])).unwrap(), NetCommand::Show);
        assert_eq!(parse(&args(&["ip", "show"])).unwrap(), NetCommand::Show);
        assert_eq!(
            parse(&args(&["ip", "mac", "02:AA:BB:CC:DD:EE"])).unwrap(),
            NetCommand::SetMac("02:aa:bb:cc:dd:ee".to_string())
        );
        assert_eq!(
            parse(&args(&["net", "network", "10.0.0.4/30"])).unwrap(),
            NetCommand::SetNetwork("10.0.0.4".to_string())
        );
        assert_eq!(
            parse(&args(&["net", "network", "172.16.1.0"])).unwrap(),
            NetCommand::SetNetwork("172.16.1.0".to_string())
        );
    }

    #[test]
    fn rejects_invalid_identities_and_legacy_subnet_grammar() {
        for value in [
            "00:11:22:33:44:55",
            "02:11:22:33:44",
            "02:11:22:33:44:gg",
            "02:11:22:33:44:55:66",
        ] {
            assert_eq!(parse(&args(&["ip", "mac", value])).unwrap_err(), USAGE);
        }
        for value in ["10.0.0.1", "10.0.0.0/24", "8.8.8.0/30", "10.0.0.256/30"] {
            assert_eq!(parse(&args(&["ip", "network", value])).unwrap_err(), USAGE);
        }
        assert_eq!(parse(&args(&["ip", "subnet"])).unwrap_err(), USAGE);
        assert_eq!(parse(&args(&["ip", "network"])).unwrap_err(), USAGE);
        assert_eq!(
            parse(&args(&["ip", "network", "10.0.0.0", "extra"])).unwrap_err(),
            USAGE
        );
    }

    #[test]
    fn show_requests_net_and_prints_identity() {
        let client = FakeTransport {
            response: envelope("get", "10.0.0.0", "10.0.0.1", "10.0.0.2"),
            ..Default::default()
        };
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = run(&client, &args(&["ip"]), false, &mut stdout, &mut stderr).unwrap();

        assert_eq!(code, 0);
        assert_eq!(
            String::from_utf8(stdout).unwrap().trim(),
            "mac=02:11:22:33:44:55 origin=derived network=10.0.0.0/30 ip=10.0.0.1 dhcp_base=10.0.0.2 url=http://10.0.0.1/"
        );
        assert!(stderr.is_empty());
        let requests = client.requests.borrow();
        assert_eq!(requests.len(), 1);
        assert_eq!(requests[0].method, Method::GET);
        assert_eq!(requests[0].path, "/api/v1/net");
    }

    #[test]
    fn set_puts_only_the_requested_identity_field() {
        let client = FakeTransport {
            response: envelope("set", "10.0.0.4", "10.0.0.5", "10.0.0.6"),
            ..Default::default()
        };
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = run(
            &client,
            &args(&["ip", "network", "10.0.0.4/30"]),
            false,
            &mut stdout,
            &mut stderr,
        )
        .unwrap();

        assert_eq!(code, 0);
        assert!(String::from_utf8(stdout)
            .unwrap()
            .contains("network=10.0.0.4/30 ip=10.0.0.5"));
        let requests = client.requests.borrow();
        assert_eq!(requests[0].method, Method::PUT);
        assert_eq!(requests[0].path, "/api/v1/net");
        assert_eq!(requests[0].body, Some(json!({ "network": "10.0.0.4" })));
    }

    #[test]
    fn set_mac_puts_only_mac() {
        let client = FakeTransport {
            response: envelope("set", "10.0.0.0", "10.0.0.1", "10.0.0.2"),
            ..Default::default()
        };
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = run(
            &client,
            &args(&["ip", "mac", "02:aa:bb:cc:dd:ee"]),
            false,
            &mut stdout,
            &mut stderr,
        )
        .unwrap();

        assert_eq!(code, 0);
        let requests = client.requests.borrow();
        assert_eq!(
            requests[0].body,
            Some(json!({ "mac": "02:aa:bb:cc:dd:ee" }))
        );
    }

    #[test]
    fn json_mode_preserves_the_firmware_envelope() {
        let raw = envelope("get", "10.0.0.0", "10.0.0.1", "10.0.0.2");
        let client = FakeTransport {
            response: raw.clone(),
            ..Default::default()
        };
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = run(
            &client,
            &args(&["ip", "--json"]),
            true,
            &mut stdout,
            &mut stderr,
        )
        .unwrap();

        assert_eq!(code, 0);
        assert_eq!(String::from_utf8(stdout).unwrap(), raw);
        assert!(stderr.is_empty());
    }

    #[test]
    fn json_http_errors_preserve_firmware_envelopes_and_fail() {
        for (status, code, message) in http_failure_cases() {
            let raw = error_envelope(code, message);
            let (client, server) = mock_http_response(status, &raw);
            let mut stdout = Vec::new();
            let mut stderr = Vec::new();

            let exit_code = run(
                &client,
                &args(&["ip", "network", "10.0.0.4", "--json"]),
                true,
                &mut stdout,
                &mut stderr,
            )
            .unwrap();

            assert_eq!(exit_code, 1);
            assert_eq!(String::from_utf8(stdout).unwrap(), raw);
            assert!(stderr.is_empty());
            let request = server.join().unwrap();
            assert!(request.starts_with("PUT /api/v1/net HTTP/1.1\r\n"));
            assert!(request.ends_with(r#"{"network":"10.0.0.4"}"#));
        }
    }

    #[test]
    fn text_http_errors_report_firmware_message_and_code() {
        for (status, code, message) in http_failure_cases() {
            let (client, server) = mock_http_response(status, &error_envelope(code, message));
            let mut stdout = Vec::new();
            let mut stderr = Vec::new();

            let exit_code = run(&client, &args(&["ip"]), false, &mut stdout, &mut stderr).unwrap();

            assert_eq!(exit_code, 1);
            assert!(stdout.is_empty());
            assert_eq!(
                String::from_utf8(stderr).unwrap(),
                format!("{message} ({code})\n")
            );
            assert!(server
                .join()
                .unwrap()
                .starts_with("GET /api/v1/net HTTP/1.1\r\n"));
        }
    }

    #[test]
    fn http_errors_preserve_non_envelope_bodies() {
        for raw in ["upstream unavailable", ""] {
            for json_output in [false, true] {
                let (client, server) = mock_http_response(StatusCode::INTERNAL_SERVER_ERROR, raw);
                let mut stdout = Vec::new();
                let mut stderr = Vec::new();

                let exit_code = run(
                    &client,
                    &args(&["ip"]),
                    json_output,
                    &mut stdout,
                    &mut stderr,
                )
                .unwrap();

                assert_eq!(exit_code, 1);
                if json_output {
                    assert_eq!(String::from_utf8(stdout).unwrap(), raw);
                    assert!(stderr.is_empty());
                } else {
                    assert!(stdout.is_empty());
                    let error = BoardHttpError {
                        status: StatusCode::INTERNAL_SERVER_ERROR,
                        body: raw.to_string(),
                    };
                    assert_eq!(String::from_utf8(stderr).unwrap(), format!("{error}\n"));
                }
                server.join().unwrap();
            }
        }
    }

    #[test]
    fn connection_refused_is_a_transport_error() {
        let server = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = server.local_addr().unwrap();
        drop(server);
        let client = BoardClient::new(&format!("http://{addr}"), Duration::from_secs(2)).unwrap();
        for json_output in [false, true] {
            let mut stdout = Vec::new();
            let mut stderr = Vec::new();

            let exit_code = run(
                &client,
                &args(&["ip"]),
                json_output,
                &mut stdout,
                &mut stderr,
            )
            .unwrap();

            assert_eq!(exit_code, 1);
            if json_output {
                let value: serde_json::Value = serde_json::from_slice(&stdout).unwrap();
                assert_eq!(value["ok"], false);
                assert_eq!(value["command"], "net");
                assert_eq!(value["error"]["code"], "transport_error");
                assert!(!value["error"]["message"].as_str().unwrap().is_empty());
                assert!(stderr.is_empty());
            } else {
                assert!(stdout.is_empty());
                assert!(!stderr.is_empty());
            }
        }
    }

    #[test]
    fn transport_error_returns_one() {
        let client = FakeTransport {
            error: Some("dial failed".to_string()),
            ..Default::default()
        };
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = run(&client, &args(&["ip"]), false, &mut stdout, &mut stderr).unwrap();

        assert_eq!(code, 1);
        assert!(String::from_utf8(stderr).unwrap().contains("dial failed"));
    }

    #[test]
    fn json_usage_error_is_structured() {
        let client = FakeTransport::default();
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = run(
            &client,
            &args(&["ip", "mac", "00:11:22:33:44:55"]),
            true,
            &mut stdout,
            &mut stderr,
        )
        .unwrap();

        assert_eq!(code, 2);
        let value: serde_json::Value = serde_json::from_slice(&stdout).unwrap();
        assert_eq!(value["ok"], false);
        assert_eq!(value["command"], "net");
        assert_eq!(value["error"]["code"], "usage");
    }

    #[test]
    fn response_with_wrong_action_is_rejected() {
        let client = FakeTransport {
            response: envelope("get", "10.0.0.0", "10.0.0.1", "10.0.0.2"),
            ..Default::default()
        };
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = run(
            &client,
            &args(&["ip", "network", "10.0.0.4"]),
            false,
            &mut stdout,
            &mut stderr,
        )
        .unwrap();

        assert_eq!(code, 1);
        assert!(String::from_utf8(stderr)
            .unwrap()
            .contains("unexpected net action"));
    }

    #[test]
    fn response_with_invalid_identity_is_rejected() {
        let client = FakeTransport {
            response: "{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":true,\"command\":\"net\",\"action\":\"get\",\"mac\":\"00:11:22:33:44:55\",\"origin\":\"derived\",\"network\":\"10.0.0.0\",\"ip\":\"10.0.0.1\",\"dhcp_base\":\"10.0.0.2\",\"url\":\"http://10.0.0.1/\"}\n".to_string(),
            ..Default::default()
        };
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = run(&client, &args(&["ip"]), false, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 1);
        assert!(String::from_utf8(stderr).unwrap().contains("invalid mac"));
    }

    #[test]
    fn usage_writer_emits_text() {
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();
        let code = run(
            &FakeTransport::default(),
            &args(&["ip", "network", "10.0.0.1"]),
            false,
            &mut stdout,
            &mut stderr,
        )
        .unwrap();
        assert_eq!(code, 2);
        assert_eq!(String::from_utf8(stderr).unwrap().trim(), USAGE);
    }
}
