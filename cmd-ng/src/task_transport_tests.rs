use crate::client::{BoardClient, BoardRequest, BoardTransport};
use crate::task_blob::{TaskBlob, TASK_MARKER_VERSION};
use crate::task_command::{TaskCommandIo, TaskRunner};
use crate::task_test_support::RecordingSleeper;
use anyhow::{anyhow, Result};
use reqwest::Method;
use serde_json::json;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::time::Duration;
use std::{cell::RefCell, fs};

struct TestFile(PathBuf);

impl TestFile {
    fn create(contents: &str) -> Result<Self> {
        let path = std::env::temp_dir().join(format!(
            "linkr-task-transport-{}.ndjson",
            uuid::Uuid::new_v4()
        ));
        fs::write(&path, contents)?;
        Ok(Self(path))
    }

    fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for TestFile {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}

fn read_http_request(stream: &mut std::net::TcpStream) -> Result<Vec<u8>> {
    let mut request = Vec::new();
    let mut chunk = [0_u8; 1024];
    let (header_end, content_length) = loop {
        let length = stream.read(&mut chunk)?;
        if length == 0 {
            return Err(anyhow!("connection closed before complete HTTP headers"));
        }
        request.extend_from_slice(&chunk[..length]);
        if let Some(offset) = request.windows(4).position(|window| window == b"\r\n\r\n") {
            let header_end = offset + 4;
            let headers = std::str::from_utf8(&request[..header_end])?;
            let content_length = headers
                .lines()
                .find_map(|line| {
                    let (name, value) = line.split_once(':')?;
                    name.eq_ignore_ascii_case("content-length")
                        .then(|| value.trim().parse::<usize>())
                })
                .transpose()?
                .unwrap_or(0);
            break (header_end, content_length);
        }
    };
    while request.len() < header_end + content_length {
        let length = stream.read(&mut chunk)?;
        if length == 0 {
            return Err(anyhow!("connection closed before complete HTTP body"));
        }
        request.extend_from_slice(&chunk[..length]);
    }
    Ok(request)
}

fn exact_boundary_blob() -> Result<String> {
    let record = json!({
        "method": "PUT",
        "path": "/api/v1/power/12v_out",
        "body": "{\"state\":\"off\"}",
    });
    let mut blob = format!("{TASK_MARKER_VERSION}\n# task exact-boundary\n{record}\n");
    while blob.len() < 4096 {
        let remaining = 4096 - blob.len();
        if remaining == 1 {
            blob.push('\n');
        } else {
            let padding = (remaining - 2).min(255);
            blob.push('#');
            blob.push_str(&"x".repeat(padding));
            blob.push('\n');
        }
    }
    TaskBlob::parse(&blob)?;
    Ok(blob)
}

#[test]
fn board_transport_send_text_preserves_generic_control_request() -> Result<()> {
    let listener = std::net::TcpListener::bind("127.0.0.1:0")?;
    let address = listener.local_addr()?;
    let (sender, receiver) = std::sync::mpsc::channel();
    let server = std::thread::spawn(move || -> Result<()> {
        let (mut stream, _) = listener.accept()?;
        let mut buffer = [0_u8; 2048];
        let length = stream.read(&mut buffer)?;
        sender.send(String::from_utf8_lossy(&buffer[..length]).into_owned())?;
        stream.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}")?;
        Ok(())
    });
    let client = BoardClient::new(&format!("http://{address}"), Duration::from_secs(2))?;

    BoardTransport::send_text(
        &client,
        BoardRequest {
            method: Method::PUT,
            path: "/api/v1/power/12v_out".to_string(),
            query: Vec::new(),
            body: Some(json!({"state": "off"})),
        },
    )?;

    let request = receiver.recv()?;
    assert!(request.starts_with("PUT /api/v1/power/12v_out HTTP/1.1"));
    assert!(request.contains("{\"state\":\"off\"}"));
    server
        .join()
        .map_err(|_| anyhow!("transport characterization server thread failed"))??;
    Ok(())
}

#[test]
fn task_store_sends_exact_4096_byte_blob_without_json_string_framing() -> Result<()> {
    // Given: a valid Task blob at the inclusive firmware byte limit.
    let blob = exact_boundary_blob()?;
    assert_eq!(blob.len(), 4096);
    let file = TestFile::create(&blob)?;
    let listener = std::net::TcpListener::bind("127.0.0.1:0")?;
    let address = listener.local_addr()?;
    let server = std::thread::spawn(move || -> Result<Vec<u8>> {
        let (mut stream, _) = listener.accept()?;
        let request = read_http_request(&mut stream)?;
        let response = json!({
            "schema": "radxa-linkr-debugger.v1",
            "ok": true,
            "command": "task",
            "action": "store",
        })
        .to_string();
        write!(
            stream,
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{}",
            response.len(),
            response
        )?;
        Ok(request)
    });
    let client = BoardClient::new(&format!("http://{address}"), Duration::from_secs(2))?;
    let events = Rc::new(RefCell::new(Vec::new()));
    let sleeper = RecordingSleeper { events };
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();

    // When: the real Task store command uploads the decoded file.
    let code = TaskRunner::new(&client, &sleeper).run(
        &[
            "task".to_string(),
            "store".to_string(),
            file.path().to_string_lossy().into_owned(),
        ],
        TaskCommandIo::new(true, &mut stdout, &mut stderr),
    )?;
    let request = server
        .join()
        .map_err(|_| anyhow!("Task store server thread failed"))??;
    let header_end = request
        .windows(4)
        .position(|window| window == b"\r\n\r\n")
        .ok_or_else(|| anyhow!("missing HTTP header terminator"))?
        + 4;

    // Then: the wire body is exactly the original 4096 UTF-8 bytes.
    assert_eq!(code, 0);
    assert!(stderr.is_empty());
    assert_eq!(&request[header_end..], blob.as_bytes());
    assert!(String::from_utf8_lossy(&request[..header_end])
        .to_ascii_lowercase()
        .contains("content-type: application/json"));
    Ok(())
}
