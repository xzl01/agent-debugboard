use crate::client::{BoardBinaryUpload, BoardClient, BoardRequest, BoardTransport};
use crate::config_command;
use crate::persistent_config::{
    ConfigItemId, ConfigItemKind, ConfigValue, PersistentConfigResponse,
};
use crate::ws_status::WsStatusSnapshot;
use anyhow::Result;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::mpsc::{self, Receiver};
use std::thread;
use std::time::Duration;

const SHOW: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":1,"items":[{"id":"power/12v_out","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":true,"apply_state":"pending"}]}"#;
const SAVE: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save","saved_items":["power/12v_out"],"confirmation_items":[],"applied_items":["power/12v_out"],"snapshot":{"present":true,"version":1},"pending":0}"#;
const CLEAR: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"clear","noop":false,"snapshot":{"present":false,"version":null},"pending":0}"#;

#[test]
fn config_client_exposes_each_firmware_operation() {
    let _show = BoardClient::config_show;
    let _save = BoardClient::config_save;
    let _clear = BoardClient::config_clear;
}

#[test]
fn config_client_forwards_all_three_requests() {
    let (url, requests) = mock_server(vec![
        (200, SHOW.to_string()),
        (200, SAVE.to_string()),
        (200, CLEAR.to_string()),
    ]);
    let client = BoardClient::new(&url, Duration::from_secs(2)).unwrap();
    let items = vec![
        ConfigItemId("power/12v_out".to_string()),
        ConfigItemId("switch/sd".to_string()),
    ];

    assert_eq!(client.config_show().unwrap().envelope.items.len(), 1);
    assert!(client.config_save(&items, true).unwrap().envelope.ok);
    assert!(client.config_clear().unwrap().envelope.ok);

    let requests = (0..3).map(|_| requests.recv().unwrap()).collect::<Vec<_>>();
    assert!(requests[0].starts_with("GET /api/v1/config HTTP/1.1"));
    assert!(requests[1].starts_with("PUT /api/v1/config HTTP/1.1"));
    assert!(
        requests[1].contains(r#"{"confirm":true,"items":["power/12v_out","switch/sd"]}"#),
        "{}",
        requests[1]
    );
    assert!(requests[2].starts_with("DELETE /api/v1/config HTTP/1.1"));
}

#[test]
fn config_response_parses_typed_values_and_keeps_unknown_items() {
    let response = PersistentConfigResponse::from_raw(r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","items":[{"id":"power/12v_out","kind":"power","current":{"state":"on"}},{"id":"switch/sd","kind":"switch","current":{"route":"target"}},{"id":"gpio/GP7","kind":"gpio","current":{"direction":"input","value":0}},{"id":"future/item","kind":"future","current":{"mode":"new"},"future_field":true}]}"#.to_string()).unwrap();

    assert!(matches!(
        response.envelope.items[0].current,
        Some(ConfigValue::Power(_))
    ));
    assert!(matches!(
        response.envelope.items[1].current,
        Some(ConfigValue::Switch(_))
    ));
    assert!(matches!(
        response.envelope.items[2].current,
        Some(ConfigValue::Gpio(_))
    ));
    assert!(
        matches!(response.envelope.items[3].kind, ConfigItemKind::Unknown(ref kind) if kind == "future")
    );
    assert!(matches!(
        response.envelope.items[3].current,
        Some(ConfigValue::Unknown(_))
    ));

    let old: WsStatusSnapshot = serde_json::from_str("{}").unwrap();
    let current: WsStatusSnapshot = serde_json::from_str(r#"{"config":{"available":true,"reason":"ready","saved_count":2,"pending_count":1,"future":true}}"#).unwrap();
    assert!(old.config.is_none());
    assert_eq!(current.config.unwrap().pending_count, 1);
}

#[test]
fn config_commands_render_human_and_exact_json_for_every_operation() {
    for (args, response, expected) in [
        (
            vec!["config", "show"],
            SHOW,
            "power/12v_out kind=power current=state=off",
        ),
        (
            vec!["config", "save", "power/12v_out"],
            SAVE,
            "saved_items=power/12v_out",
        ),
        (vec!["config", "clear"], CLEAR, "current hardware unchanged"),
    ] {
        let (code, stdout, stderr, _) = run_command(&args, false, 200, response);
        assert_eq!(code, 0, "args={args:?}");
        assert!(stderr.is_empty());
        assert!(stdout.contains(expected), "stdout={stdout}");

        let (code, stdout, stderr, _) = run_command(&args, true, 200, response);
        assert_eq!(code, 0, "args={args:?}");
        assert!(stderr.is_empty());
        assert_eq!(stdout.trim(), response);
    }
}

#[test]
fn config_commands_reject_empty_selection_and_unknown_verbs_before_io() {
    let client = NoIoClient;
    for args in [vec!["config", "save"], vec!["config", "apply"]] {
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();
        let args = args.into_iter().map(str::to_string).collect::<Vec<_>>();
        let code = config_command::run(&client, &args, false, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 2);
        assert!(!String::from_utf8(stderr).unwrap().is_empty());
    }
}

#[test]
fn config_save_parser_preserves_item_order_and_explicit_confirmation() {
    let args = ["config", "save", "--confirm", "switch/sd", "power/12v_out"]
        .into_iter()
        .map(str::to_string)
        .collect::<Vec<_>>();
    let command = config_command::parse(&args).unwrap();
    match command {
        config_command::ConfigCommand::Save { items, confirm } => {
            assert!(confirm);
            assert_eq!(
                items.iter().map(ConfigItemId::as_str).collect::<Vec<_>>(),
                ["switch/sd", "power/12v_out"]
            );
        }
        _ => panic!("expected config save"),
    }
    let (_, _, _, request) = run_command(
        &["config", "save", "--confirm", "switch/sd"],
        false,
        200,
        SAVE,
    );
    assert!(request.contains(r#"{"confirm":true,"items":["switch/sd"]}"#));
}

#[test]
fn config_commands_preserve_confirmation_busy_and_partial_failure_details() {
    let confirmation = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"confirmation_required","message":"confirmation is required"},"dangerous_items":["switch/usb"]}"#;
    let busy = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"clear","error":{"code":"busy","message":"blocked"},"activity":"capture"}"#;
    let partial = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"apply_failed","message":"failed"},"applied_items":["power/12v_out"],"failed_item":"switch/sd","pending_items":["switch/sd"]}"#;

    let (code, stdout, stderr, request) =
        run_command(&["config", "save", "switch/usb"], true, 409, confirmation);
    assert_eq!(code, 1);
    assert!(stderr.is_empty());
    assert_eq!(stdout.trim(), confirmation);
    assert!(
        request.contains(r#"{"confirm":false,"items":["switch/usb"]}"#),
        "{request}"
    );

    let (code, _, stderr, _) = run_command(&["config", "clear"], false, 409, busy);
    assert_eq!(code, 1);
    assert!(stderr.contains("busy: blocked activity=capture"));

    let (code, _, stderr, request) = run_command(
        &["config", "save", "--confirm", "power/12v_out"],
        false,
        500,
        partial,
    );
    assert_eq!(code, 1);
    assert!(stderr.contains("apply_failed: failed applied_items=power/12v_out failed_item=switch/sd pending_items=switch/sd"));
    assert!(request.contains(r#"{"confirm":true,"items":["power/12v_out"]}"#));
}

#[test]
fn board_client_rejects_malformed_success_and_error_envelopes() {
    let show = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":false,"version":null},"pending":0}"#;
    let (url, requests) = mock_server(vec![(200, show.to_string())]);
    let client = BoardClient::new(&url, Duration::from_secs(2)).unwrap();
    assert!(client.config_show().is_err());
    let _ = requests.recv().unwrap();

    let save = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"confirmation_required","message":"confirm"}}"#;
    let (url, requests) = mock_server(vec![(409, save.to_string())]);
    let client = BoardClient::new(&url, Duration::from_secs(2)).unwrap();
    assert!(client
        .config_save(&[ConfigItemId("future/item".to_string())], false)
        .is_err());
    let _ = requests.recv().unwrap();

    let partial = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"apply_failed","message":"failed"},"applied_items":[],"failed_item":null}"#;
    let (url, requests) = mock_server(vec![(500, partial.to_string())]);
    let client = BoardClient::new(&url, Duration::from_secs(2)).unwrap();
    assert!(client
        .config_save(&[ConfigItemId("power/12v_out".to_string())], true)
        .is_err());
    let _ = requests.recv().unwrap();
}

#[test]
fn malformed_config_response_becomes_cli_error_without_rendering_defaults() {
    let body = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":false,"version":null},"pending":0}"#;
    let (code, stdout, stderr, _) = run_command(&["config", "show"], false, 200, body);

    assert_eq!(code, 1);
    assert!(stdout.is_empty());
    assert!(stderr.contains("config"));
    assert!(!stderr.contains("pending=0"));

    let (code, stdout, stderr, _) = run_command(&["config", "show"], true, 200, body);
    assert_eq!(code, 1);
    assert!(stderr.is_empty());
    assert!(stdout.contains(r#""code":"transport_error""#));
    assert!(!stdout.contains(r#""action":"get""#));
}

#[test]
fn config_json_preserves_valid_raw_firmware_bytes() {
    let body = format!(" \n{SHOW}\n");
    let (code, stdout, stderr, _) = run_command(&["config", "show"], true, 200, &body);

    assert_eq!(code, 0);
    assert_eq!(stdout, body);
    assert!(stderr.is_empty());
}

fn run_command(args: &[&str], json: bool, status: u16, body: &str) -> (u8, String, String, String) {
    let (url, requests) = mock_server(vec![(status, body.to_string())]);
    let client = BoardClient::new(&url, Duration::from_secs(2)).unwrap();
    let mut args = args
        .iter()
        .map(|arg| (*arg).to_string())
        .collect::<Vec<_>>();
    if json {
        args.push("--json".to_string());
    }
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();
    let code = config_command::run(&client, &args, json, &mut stdout, &mut stderr).unwrap();
    (
        code,
        String::from_utf8(stdout).unwrap(),
        String::from_utf8(stderr).unwrap(),
        requests.recv().unwrap(),
    )
}

fn mock_server(responses: Vec<(u16, String)>) -> (String, Receiver<String>) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let address = listener.local_addr().unwrap();
    let (sender, receiver) = mpsc::channel();
    thread::spawn(move || {
        for (status, body) in responses {
            let (mut stream, _) = listener.accept().unwrap();
            let request = read_request(&mut stream);
            sender.send(request).unwrap();
            let response = format!(
                "HTTP/1.1 {status} Response\r\nConnection: close\r\nContent-Length: {}\r\n\r\n{body}",
                body.len()
            );
            stream.write_all(response.as_bytes()).unwrap();
        }
    });
    (format!("http://{address}"), receiver)
}

fn read_request(stream: &mut TcpStream) -> String {
    let mut data = Vec::new();
    let mut buffer = [0_u8; 1024];
    loop {
        let read = stream.read(&mut buffer).unwrap();
        assert_ne!(read, 0);
        data.extend_from_slice(&buffer[..read]);
        if let Some(end) = data.windows(4).position(|window| window == b"\r\n\r\n") {
            let headers = String::from_utf8_lossy(&data[..end]);
            let length = headers.lines().find_map(|line| {
                line.split_once(':')
                    .filter(|(key, _)| key.eq_ignore_ascii_case("content-length"))
                    .and_then(|(_, value)| value.trim().parse::<usize>().ok())
            });
            let chunked = headers.lines().any(|line| {
                line.split_once(':').is_some_and(|(key, value)| {
                    key.eq_ignore_ascii_case("transfer-encoding")
                        && value.trim().eq_ignore_ascii_case("chunked")
                })
            });
            if length.is_some_and(|length| data.len() >= end + 4 + length)
                || (chunked && data[end + 4..].ends_with(b"0\r\n\r\n"))
                || (length.is_none() && !chunked)
            {
                return String::from_utf8(data).unwrap();
            }
        }
    }
}

struct NoIoClient;

impl BoardTransport for NoIoClient {
    fn send_text(&self, _: BoardRequest) -> Result<String> {
        panic!("unexpected I/O")
    }
    fn send_raw_json(&self, _: crate::client::BoardRawJsonRequest) -> Result<String> {
        panic!("unexpected I/O")
    }
    fn upload_binary(&self, _: BoardBinaryUpload) -> Result<String> {
        panic!("unexpected I/O")
    }
    fn base_url(&self) -> &str {
        "http://unused.invalid"
    }
}
