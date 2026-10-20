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

const USAGE: &str = "usage: radxa-linkr-debuggerctl ip subnet [<1-254>]";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum NetCommand {
    Show,
    Set(u8),
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
    if cleaned.get(1).map(|arg| arg.as_str()) != Some("subnet") {
        return Err(USAGE.to_string());
    }

    match cleaned.len() {
        2 => Ok(NetCommand::Show),
        3 => {
            let subnet = cleaned[2].parse::<u8>().map_err(|_| USAGE.to_string())?;
            if !(1..=254).contains(&subnet) {
                return Err(USAGE.to_string());
            }
            Ok(NetCommand::Set(subnet))
        }
        _ => Err(USAGE.to_string()),
    }
}

struct NetStatus {
    subnet: u8,
    ip: String,
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

    let subnet = value
        .get("subnet")
        .and_then(Value::as_u64)
        .filter(|subnet| (1..=254).contains(subnet))
        .ok_or_else(|| anyhow::anyhow!("invalid subnet in net response"))? as u8;
    let ip = value
        .get("ip")
        .and_then(Value::as_str)
        .filter(|ip| !ip.is_empty())
        .ok_or_else(|| anyhow::anyhow!("invalid ip in net response"))?;

    Ok(NetStatus {
        subnet,
        ip: ip.to_string(),
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
        NetCommand::Set(subnet) => (
            BoardRequest {
                method: Method::PUT,
                path: "/api/v1/net".to_string(),
                query: vec![],
                body: Some(serde_json::json!({ "subnet": subnet })),
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
            "subnet={} ip={} url=http://{}/",
            status.subnet, status.ip, status.ip
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
        BoardBinaryUpload, BoardClient, BoardHttpError, BoardRawJsonRequest, BoardRequest,
        BoardTransport,
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

        fn send_raw_json(&self, _request: BoardRawJsonRequest) -> Result<String> {
            unreachable!()
        }

        fn upload_binary(&self, _request: BoardBinaryUpload) -> Result<String> {
            unreachable!()
        }

        fn base_url(&self) -> &str {
            "http://172.29.203.1"
        }
    }

    fn envelope(action: &str, subnet: u8, ip: &str) -> String {
        format!(
            "{{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":true,\"command\":\"net\",\"action\":\"{action}\",\"subnet\":{subnet},\"ip\":\"{ip}\",\"reconnect_required\":{}}}\n",
            action == "set"
        )
    }

    fn http_failure_cases() -> [(StatusCode, &'static str, &'static str); 2] {
        [
            (StatusCode::BAD_REQUEST, "invalid_subnet", "Subnet rejected"),
            (
                StatusCode::INTERNAL_SERVER_ERROR,
                "settings_write_failed",
                "Could not save subnet",
            ),
        ]
    }

    fn error_envelope(code: &str, message: &str) -> String {
        format!(
            " {{ \"schema\":\"radxa-linkr-debugger.v1\",\"ok\":false,\"command\":\"net\",\"error\":{{\"code\":\"{code}\",\"message\":\"{message}\"}} }} \n"
        )
    }

    #[test]
    fn parses_show_and_set_for_ip_and_net_aliases() {
        assert_eq!(parse(&args(&["ip", "subnet"])).unwrap(), NetCommand::Show);
        assert_eq!(
            parse(&args(&["ip", "subnet", "204"])).unwrap(),
            NetCommand::Set(204)
        );
        assert_eq!(parse(&args(&["net", "subnet"])).unwrap(), NetCommand::Show);
        assert_eq!(
            parse(&args(&["net", "subnet", "1"])).unwrap(),
            NetCommand::Set(1)
        );
    }

    #[test]
    fn rejects_out_of_range_and_malformed_subnets() {
        for value in ["0", "255", "abc", "1.5", "-1"] {
            assert_eq!(parse(&args(&["ip", "subnet", value])).unwrap_err(), USAGE);
        }
        assert_eq!(parse(&args(&["ip"])).unwrap_err(), USAGE);
        assert_eq!(
            parse(&args(&["ip", "subnet", "1", "2"])).unwrap_err(),
            USAGE
        );
    }

    #[test]
    fn show_requests_net_and_prints_state() {
        let client = FakeTransport {
            response: envelope("get", 203, "172.29.203.1"),
            ..Default::default()
        };
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = run(
            &client,
            &args(&["ip", "subnet"]),
            false,
            &mut stdout,
            &mut stderr,
        )
        .unwrap();

        assert_eq!(code, 0);
        assert_eq!(
            String::from_utf8(stdout).unwrap().trim(),
            "subnet=203 ip=172.29.203.1 url=http://172.29.203.1/"
        );
        assert!(stderr.is_empty());
        let requests = client.requests.borrow();
        assert_eq!(requests.len(), 1);
        assert_eq!(requests[0].method, Method::GET);
        assert_eq!(requests[0].path, "/api/v1/net");
    }

    #[test]
    fn set_puts_subnet_and_prints_new_state() {
        let client = FakeTransport {
            response: envelope("set", 204, "172.29.204.1"),
            ..Default::default()
        };
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = run(
            &client,
            &args(&["ip", "subnet", "204"]),
            false,
            &mut stdout,
            &mut stderr,
        )
        .unwrap();

        assert_eq!(code, 0);
        assert_eq!(
            String::from_utf8(stdout).unwrap().trim(),
            "subnet=204 ip=172.29.204.1 url=http://172.29.204.1/"
        );
        let requests = client.requests.borrow();
        assert_eq!(requests[0].method, Method::PUT);
        assert_eq!(requests[0].path, "/api/v1/net");
        assert_eq!(requests[0].body, Some(json!({ "subnet": 204 })));
    }

    #[test]
    fn json_mode_preserves_the_firmware_envelope() {
        let raw = envelope("get", 203, "172.29.203.1");
        let client = FakeTransport {
            response: raw.clone(),
            ..Default::default()
        };
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = run(
            &client,
            &args(&["ip", "subnet", "--json"]),
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
                &args(&["ip", "subnet", "204", "--json"]),
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
            assert!(request.ends_with(r#"{"subnet":204}"#));
        }
    }

    #[test]
    fn text_http_errors_report_firmware_message_and_code() {
        for (status, code, message) in http_failure_cases() {
            let (client, server) = mock_http_response(status, &error_envelope(code, message));
            let mut stdout = Vec::new();
            let mut stderr = Vec::new();

            let exit_code = run(
                &client,
                &args(&["ip", "subnet"]),
                false,
                &mut stdout,
                &mut stderr,
            )
            .unwrap();

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
                    &args(&["ip", "subnet"]),
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
                &args(&["ip", "subnet"]),
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

        let code = run(
            &client,
            &args(&["ip", "subnet"]),
            false,
            &mut stdout,
            &mut stderr,
        )
        .unwrap();

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
            &args(&["ip", "subnet", "255"]),
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
            response: envelope("get", 203, "172.29.203.1"),
            ..Default::default()
        };
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = run(
            &client,
            &args(&["ip", "subnet", "204"]),
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
    fn usage_writer_emits_text() {
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();
        let code = run(
            &FakeTransport::default(),
            &args(&["ip"]),
            false,
            &mut stdout,
            &mut stderr,
        )
        .unwrap();
        assert_eq!(code, 2);
        assert_eq!(String::from_utf8(stderr).unwrap().trim(), USAGE);
    }
}
