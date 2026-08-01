use super::config_io::execute_request;
use super::config_result::{ConfigJobKind, ConfigOutcome};
use super::config_state::{ConfigRequest, SavedConfigState};
use crate::client::BoardClient;
use crate::persistent_config::ConfigItemId;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::mpsc::{self, Receiver};
use std::thread;
use std::time::Duration;

const SHOW: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":2},"pending":0,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":true,"apply_state":"applied"}]}"#;
const SAVE: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save","saved_items":["power/alpha"],"confirmation_items":["power/alpha"],"snapshot":{"present":true,"version":2},"pending":0}"#;
const CONFIRM: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"confirmation_required","message":"confirm"},"dangerous_items":["power/alpha"]}"#;
const BUSY: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"apply","error":{"code":"busy","message":"blocked"},"activity":"capture"}"#;
const STORAGE: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"clear","error":{"code":"storage_error","message":"flash"}}"#;

enum Reply {
    Http(u16, &'static str),
    Disconnect,
}

#[test]
fn success_sends_confirmed_save_then_authoritative_get() {
    let (url, requests) = mock_server(vec![Reply::Http(200, SAVE), Reply::Http(200, SHOW)]);
    let client = BoardClient::new(&url, Duration::from_secs(1)).unwrap();
    let request = ConfigRequest::Save {
        items: vec![ConfigItemId("power/alpha".to_string())],
        confirm: true,
    };

    let result = execute_request(&client, request);

    assert!(result.mutation.as_ref().is_some_and(Result::is_ok));
    assert!(result.refresh.is_ok());
    let mutation = requests.recv().unwrap();
    let refresh = requests.recv().unwrap();
    assert!(mutation.starts_with("PUT /api/v1/config HTTP/1.1"));
    assert!(mutation.contains(r#"{"confirm":true,"items":["power/alpha"]}"#));
    assert!(refresh.starts_with("GET /api/v1/config HTTP/1.1"));
}

#[test]
fn disconnect_reports_both_attempts_without_repeating_mutation() {
    let (url, requests) = mock_server(vec![Reply::Disconnect, Reply::Disconnect]);
    let client = BoardClient::new(&url, Duration::from_secs(1)).unwrap();

    let result = execute_request(&client, ConfigRequest::Clear);

    assert!(result.mutation.as_ref().is_some_and(Result::is_err));
    assert!(result.refresh.is_err());
    assert!(requests
        .recv()
        .unwrap()
        .starts_with("DELETE /api/v1/config"));
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/config"));
    assert!(requests.recv_timeout(Duration::from_millis(50)).is_err());
}

#[test]
fn busy_response_is_preserved_after_authoritative_get() {
    let (url, requests) = mock_server(vec![Reply::Http(409, BUSY), Reply::Http(200, SHOW)]);
    let client = BoardClient::new(&url, Duration::from_secs(1)).unwrap();
    let request = ConfigRequest::Apply { confirm: false };
    let result = execute_request(&client, request.clone());
    let mut state = SavedConfigState::default();
    state.start(ConfigJobKind::Apply);

    let outcome = state.finish(result);

    assert_eq!(outcome, ConfigOutcome::Failed);
    assert!(state
        .error
        .as_deref()
        .is_some_and(|error| error.contains("busy")));
    assert!(requests
        .recv()
        .unwrap()
        .starts_with("POST /api/v1/config/apply"));
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/config"));
}

#[test]
fn storage_error_is_preserved_after_authoritative_get() {
    let (url, requests) = mock_server(vec![Reply::Http(500, STORAGE), Reply::Http(200, SHOW)]);
    let client = BoardClient::new(&url, Duration::from_secs(1)).unwrap();
    let mut state = SavedConfigState::default();
    state.start(ConfigJobKind::Clear);

    let outcome = state.finish(execute_request(&client, ConfigRequest::Clear));

    assert_eq!(outcome, ConfigOutcome::Failed);
    assert!(state
        .error
        .as_deref()
        .is_some_and(|error| error.contains("storage_error")));
    assert!(requests
        .recv()
        .unwrap()
        .starts_with("DELETE /api/v1/config"));
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/config"));
}

#[test]
fn confirmation_response_never_silently_retries_with_confirm_true() {
    let (url, requests) = mock_server(vec![Reply::Http(409, CONFIRM), Reply::Http(200, SHOW)]);
    let client = BoardClient::new(&url, Duration::from_secs(1)).unwrap();
    let request = ConfigRequest::Save {
        items: vec![ConfigItemId("power/alpha".to_string())],
        confirm: false,
    };
    let mut state = SavedConfigState::default();
    state.start(ConfigJobKind::Save);

    let outcome = state.finish(execute_request(&client, request));

    assert_eq!(outcome, ConfigOutcome::AwaitingConfirmation);
    let mutation = requests.recv().unwrap();
    assert!(mutation.contains(r#""confirm":false"#));
    assert!(!mutation.contains(r#""confirm":true"#));
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/config"));
    assert!(requests.recv_timeout(Duration::from_millis(50)).is_err());
}

fn mock_server(replies: Vec<Reply>) -> (String, Receiver<String>) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let address = listener.local_addr().unwrap();
    let (sender, receiver) = mpsc::channel();
    thread::spawn(move || {
        for reply in replies {
            let (mut stream, _) = listener.accept().unwrap();
            sender.send(read_request(&mut stream)).unwrap();
            if let Reply::Http(status, body) = reply {
                let response = format!(
                    "HTTP/1.1 {status} Response\r\nConnection: close\r\nContent-Length: {}\r\n\r\n{body}",
                    body.len()
                );
                stream.write_all(response.as_bytes()).unwrap();
            }
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
            if length.is_none_or(|length| data.len() >= end + 4 + length) {
                return String::from_utf8(data).unwrap();
            }
        }
    }
}
