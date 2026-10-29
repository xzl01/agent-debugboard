use crate::client::{
    ota_upload_timeout, BoardBinaryUpload, BoardClient, BoardHttpError, BoardRequest, BoardTarget,
    BoardTransport,
};
use anyhow::Context;
use reqwest::{Method, StatusCode};
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
fn non_success_responses_retain_status_and_raw_body() {
    let body =
        " \n{\"ok\":false,\"error\":{\"code\":\"rejected\",\"message\":\"Request rejected\"}}\n ";
    for status in [StatusCode::BAD_REQUEST, StatusCode::INTERNAL_SERVER_ERROR] {
        let (client, server) = mock_http_response(status, body);
        let response = client.send_text(BoardRequest {
            method: Method::GET,
            path: "/api/v1/net".to_string(),
            query: vec![],
            body: None,
        });
        let error = response.unwrap_err();
        assert_eq!(error.to_string(), format!("HTTP {status}: {}", body.trim()));
        let error = Err::<(), _>(error).context("request failed").unwrap_err();
        let error = error.downcast_ref::<BoardHttpError>().unwrap();
        assert_eq!(error.status, status);
        assert_eq!(error.body, body);
        server.join().unwrap();
    }
}

#[test]
fn http_error_display_keeps_existing_empty_body_format() {
    for body in ["", " \n\t"] {
        let error = BoardHttpError {
            status: StatusCode::INTERNAL_SERVER_ERROR,
            body: body.to_string(),
        };
        assert_eq!(
            error.to_string(),
            "HTTP 500 Internal Server Error: 500 Internal Server Error"
        );
    }
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

pub(crate) fn mock_http_response(
    status: StatusCode,
    body: &str,
) -> (BoardClient, std::thread::JoinHandle<String>) {
    let server = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = server.local_addr().unwrap();
    let response = format!(
        "HTTP/1.1 {status}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    );
    let server = std::thread::spawn(move || {
        let (mut stream, _) = server.accept().unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        stream
            .set_write_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        let request = read_full_http_request(&mut stream);
        stream.write_all(response.as_bytes()).unwrap();
        request
    });
    let client = BoardClient::new(&format!("http://{addr}"), Duration::from_secs(2)).unwrap();
    (client, server)
}

pub(crate) fn read_full_http_request(stream: &mut std::net::TcpStream) -> String {
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
                content_length = Some(
                    header_value(&headers, "content-length")
                        .map(|value| value.parse::<usize>().unwrap())
                        .unwrap_or(0),
                );
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

pub(crate) fn response_server(
    response: String,
) -> (
    String,
    std::thread::JoinHandle<(String, std::net::SocketAddr)>,
) {
    let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("http://{}", listener.local_addr().unwrap());
    let handle = std::thread::spawn(move || {
        let (mut stream, peer) = listener.accept().unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        stream
            .set_write_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        let request = read_full_http_request(&mut stream);
        stream.write_all(response.as_bytes()).unwrap();
        (request, peer)
    });
    (url, handle)
}

fn status_request() -> BoardRequest {
    BoardRequest {
        method: Method::GET,
        path: "/api/v1/status".into(),
        query: vec![],
        body: None,
    }
}

fn isolated_client_test(name: &str, marker: &str) -> std::process::Command {
    let mut child = std::process::Command::new(std::env::current_exe().unwrap());
    child
        .args(["--exact", name, "--nocapture"])
        .env(marker, "1")
        .env("NO_PROXY", "*")
        .env("no_proxy", "*")
        .env_remove("REQUEST_METHOD");
    for name in [
        "HTTP_PROXY",
        "http_proxy",
        "HTTPS_PROXY",
        "https_proxy",
        "ALL_PROXY",
        "all_proxy",
    ] {
        child.env_remove(name);
    }
    child
}

#[test]
fn discovered_target_client_bypasses_proxy_environment() {
    const CHILD: &str = "LINKR_TEST_DIRECT_PROXY_CHILD";
    const ORIGIN: &str = "LINKR_TEST_DIRECT_PROXY_ORIGIN";
    if std::env::var_os(CHILD).is_some() {
        let url = std::env::var(ORIGIN).unwrap();
        let target = BoardTarget::direct(url.clone(), std::net::Ipv4Addr::LOCALHOST);
        let direct = BoardClient::from_target(&target, Duration::from_secs(2)).unwrap();
        assert_eq!(direct.target(), target);
        assert_eq!(direct.send_text(status_request()).unwrap(), "direct");
        // Prove the hostile environment is effective, rather than relying on
        // a platform's implicit localhost proxy bypass.
        let configured = BoardClient::new(&url, Duration::from_secs(2)).unwrap();
        assert!(configured.target().local_ip.is_none());
        assert_eq!(configured.send_text(status_request()).unwrap(), "proxied");
        return;
    }

    let (origin, origin_server) = response_server(
        "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\ndirect".into(),
    );
    let (proxy, proxy_server) = response_server(
        "HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\nproxied".into(),
    );
    // Change environment only in an isolated test subprocess: parallel unit
    // tests must never inherit temporary proxy settings.
    let mut child = isolated_client_test(
        "client_tests::discovered_target_client_bypasses_proxy_environment",
        CHILD,
    );
    child
        .env(ORIGIN, &origin)
        .env("NO_PROXY", "")
        .env("no_proxy", "")
        .env_remove("REQUEST_METHOD");
    for name in [
        "HTTP_PROXY",
        "http_proxy",
        "HTTPS_PROXY",
        "https_proxy",
        "ALL_PROXY",
        "all_proxy",
    ] {
        child.env(name, &proxy);
    }
    let output = child.output().unwrap();
    assert!(
        output.status.success(),
        "stdout={}\nstderr={}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    let (request, peer) = origin_server.join().unwrap();
    assert!(request.starts_with("GET /api/v1/status HTTP/1.1"));
    assert_eq!(
        peer.ip(),
        std::net::IpAddr::V4(std::net::Ipv4Addr::LOCALHOST)
    );
    let (request, _) = proxy_server.join().unwrap();
    assert!(request.starts_with(&format!("GET {origin}/api/v1/status HTTP/1.1")));
}

#[test]
fn discovered_target_client_rejects_redirects_while_configured_urls_still_follow() {
    const CHILD: &str = "LINKR_TEST_DIRECT_REDIRECT_CHILD";
    if std::env::var_os(CHILD).is_none() {
        let output = isolated_client_test(
            "client_tests::discovered_target_client_rejects_redirects_while_configured_urls_still_follow", CHILD,
        ).output().unwrap();
        assert!(
            output.status.success(),
            "stdout={}\nstderr={}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
        return;
    }
    let response =
        || "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK".to_string();
    let unused_redirect = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    unused_redirect.set_nonblocking(true).unwrap();
    let redirect = |url: &str| {
        format!("HTTP/1.1 307 Temporary Redirect\r\nLocation: {url}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
    };
    let (origin, server) = response_server(redirect(&format!(
        "http://{}",
        unused_redirect.local_addr().unwrap()
    )));
    let target = BoardTarget::direct(origin, std::net::Ipv4Addr::LOCALHOST);
    let client = BoardClient::from_target(target, Duration::from_secs(1)).unwrap();
    let error = client.send_text(status_request()).unwrap_err();
    assert_eq!(
        error.downcast_ref::<BoardHttpError>().unwrap().status,
        StatusCode::TEMPORARY_REDIRECT
    );
    server.join().unwrap();
    assert_eq!(
        unused_redirect.accept().unwrap_err().kind(),
        std::io::ErrorKind::WouldBlock
    );

    let (destination, destination_server) = response_server(response());
    let (origin, origin_server) = response_server(redirect(&destination));
    let client = BoardClient::new(&origin, Duration::from_secs(1)).unwrap();
    assert_eq!(client.send_text(status_request()).unwrap(), "OK");
    origin_server.join().unwrap();
    destination_server.join().unwrap();
}
