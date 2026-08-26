use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::mpsc::{self, Receiver};
use std::sync::{Arc, Barrier};
use std::thread;
use std::time::Duration;

pub(super) enum Reply {
    Http(u16, &'static str),
    Slow(u16, &'static str),
    Gated(u16, &'static str, Arc<Barrier>),
    Disconnect,
}

pub(super) fn mock_server(replies: Vec<Reply>) -> (String, Receiver<String>) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let address = listener.local_addr().unwrap();
    let (sender, receiver) = mpsc::channel();
    thread::spawn(move || {
        for reply in replies {
            let (mut stream, _) = listener.accept().unwrap();
            sender.send(read_request(&mut stream)).unwrap();
            match reply {
                Reply::Http(status, body) => write_response(&mut stream, status, body),
                Reply::Slow(status, body) => {
                    thread::sleep(Duration::from_millis(300));
                    write_response(&mut stream, status, body);
                }
                Reply::Gated(status, body, gate) => {
                    gate.wait();
                    write_response(&mut stream, status, body);
                }
                Reply::Disconnect => {}
            }
        }
    });
    (format!("http://{address}"), receiver)
}

fn write_response(stream: &mut TcpStream, status: u16, body: &str) {
    let response = format!(
        "HTTP/1.1 {status} Response\r\nConnection: close\r\nContent-Length: {}\r\n\r\n{body}",
        body.len()
    );
    stream.write_all(response.as_bytes()).unwrap();
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
