use crate::client::{ota_upload_timeout, BoardBinaryUpload, BoardClient, BoardRequest};
use reqwest::Method;
use std::io::{Read, Write};
use std::time::Duration;

#[test]
fn ota_upload_timeout_has_a_sixty_second_floor() {
    assert_eq!(
        ota_upload_timeout(Duration::from_secs(2)),
        Duration::from_secs(60)
    );
    assert_eq!(
        ota_upload_timeout(Duration::from_secs(90)),
        Duration::from_secs(90)
    );
}

#[test]
fn preserves_base_path_when_sending_request() {
    let server = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = server.local_addr().unwrap();
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let (mut stream, _) = server.accept().unwrap();
        let mut buf = [0u8; 2048];
        let n = stream.read(&mut buf).unwrap();
        tx.send(String::from_utf8_lossy(&buf[..n]).to_string())
            .unwrap();
        stream
            .write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}")
            .unwrap();
    });

    let client =
        BoardClient::new(&format!("http://{addr}/prefix"), Duration::from_secs(2)).unwrap();
    client
        .send_text(BoardRequest {
            method: Method::GET,
            path: "/api/v1/status".to_string(),
            query: vec![],
            body: None,
        })
        .unwrap();

    let raw = rx.recv().unwrap();
    assert!(
        raw.starts_with("GET /prefix/api/v1/status HTTP/1.1"),
        "{raw}"
    );
}

#[test]
fn upload_binary_sends_streamed_body_with_headers() {
    let server = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = server.local_addr().unwrap();
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let (mut stream, _) = server.accept().unwrap();
        let request = read_full_http_request(&mut stream);
        tx.send(request).unwrap();
        let body =
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"ota","action":"upload"}"#;
        let response = format!(
            "HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n{}",
            body.len(),
            body
        );
        stream.write_all(response.as_bytes()).unwrap();
    });

    let path = temp_file_path("ota-upload-body");
    std::fs::write(&path, b"abc123").unwrap();
    let file = std::fs::File::open(&path).unwrap();
    let client =
        BoardClient::new(&format!("http://{addr}/prefix"), Duration::from_secs(2)).unwrap();
    let output = client
        .upload_binary(BoardBinaryUpload {
            path: "/api/v1/ota/upload".to_string(),
            file,
            size: 6,
            sha256: "6ca13d52ca70c883e0f0bb101e425a89e8624de51db2d2392593af6a84118090".to_string(),
        })
        .unwrap();
    let _ = std::fs::remove_file(&path);

    assert!(output.contains(r#""command":"ota""#));
    let raw = rx.recv().unwrap();
    assert!(
        raw.starts_with("POST /prefix/api/v1/ota/upload HTTP/1.1"),
        "{raw}"
    );
    assert_header(&raw, "content-type", "application/octet-stream");
    assert_header(&raw, "accept", "application/json");
    assert_header(&raw, "content-length", "6");
    assert_header(&raw, "x-linkr-ota-size", "6");
    assert_header(
        &raw,
        "x-linkr-ota-sha256",
        "6ca13d52ca70c883e0f0bb101e425a89e8624de51db2d2392593af6a84118090",
    );
    assert!(raw.ends_with("\r\n\r\nabc123"), "{raw}");
}

#[test]
fn upload_binary_preserves_non_success_json_body() {
    let server = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = server.local_addr().unwrap();
    std::thread::spawn(move || {
        let (mut stream, _) = server.accept().unwrap();
        let _ = read_full_http_request(&mut stream);
        let body = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"ota","error":{"code":"image_too_large","message":"OTA upload failed validation"}}"#;
        let response = format!(
            "HTTP/1.1 413 Payload Too Large\r\nContent-Length: {}\r\n\r\n{}",
            body.len(),
            body
        );
        stream.write_all(response.as_bytes()).unwrap();
    });

    let path = temp_file_path("ota-upload-error-body");
    std::fs::write(&path, b"abc123").unwrap();
    let file = std::fs::File::open(&path).unwrap();
    let client = BoardClient::new(&format!("http://{addr}"), Duration::from_secs(2)).unwrap();
    let output = client
        .upload_binary(BoardBinaryUpload {
            path: "/api/v1/ota/upload".to_string(),
            file,
            size: 6,
            sha256: "6ca13d52ca70c883e0f0bb101e425a89e8624de51db2d2392593af6a84118090".to_string(),
        })
        .unwrap();
    let _ = std::fs::remove_file(&path);

    assert!(output.contains(r#""code":"image_too_large""#));
}

fn read_full_http_request(stream: &mut std::net::TcpStream) -> String {
    let mut data = Vec::new();
    let mut buf = [0u8; 1024];
    let mut content_length = None;
    loop {
        let n = stream.read(&mut buf).unwrap();
        assert_ne!(n, 0, "connection closed before full request");
        data.extend_from_slice(&buf[..n]);
        if content_length.is_none() {
            if let Some(header_end) = find_header_end(&data) {
                let headers = String::from_utf8_lossy(&data[..header_end]);
                content_length = header_value(&headers, "content-length")
                    .and_then(|value| value.parse::<usize>().ok());
            }
        }
        if let (Some(header_end), Some(len)) = (find_header_end(&data), content_length) {
            if data.len() >= header_end + 4 + len {
                break;
            }
        }
    }
    String::from_utf8_lossy(&data).to_string()
}

fn find_header_end(data: &[u8]) -> Option<usize> {
    data.windows(4).position(|window| window == b"\r\n\r\n")
}

fn header_value<'a>(headers: &'a str, name: &str) -> Option<&'a str> {
    headers.lines().find_map(|line| {
        let (key, value) = line.split_once(':')?;
        key.eq_ignore_ascii_case(name).then_some(value.trim())
    })
}

fn assert_header(raw: &str, name: &str, expected: &str) {
    let headers = raw.split("\r\n\r\n").next().unwrap();
    assert_eq!(header_value(headers, name), Some(expected), "{raw}");
}

fn temp_file_path(name: &str) -> std::path::PathBuf {
    let mut path = std::env::temp_dir();
    path.push(format!(
        "radxa-linkr-debugger-client-test-{name}-{}",
        std::process::id(),
    ));
    path
}
