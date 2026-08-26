use super::board_io::{perform_control_action, set_gpio_input, set_gpio_output, set_switch_route};
use super::mock_board::{mock_server, Reply};
use super::TuiActionMsg;
use anyhow::Result;
use std::io::ErrorKind;
use std::net::TcpListener;
use std::sync::mpsc::{self, Receiver};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

const TIMEOUT: Duration = Duration::from_secs(1);
const OK: &str = r#"{"ok":true}"#;
const VALID_NAMES: [(&str, &str); 5] = [
    ("a/b", "a%2Fb"),
    ("a.b", "a.b"),
    ("a%2Fb", "a%252Fb"),
    ("a?b", "a%3Fb"),
    ("a#b", "a%23b"),
];

#[derive(Clone, Copy)]
enum BoardIoOperation {
    Power,
    Switch,
    GpioOutput,
    GpioInput,
}

impl BoardIoOperation {
    const ALL: [Self; 4] = [Self::Power, Self::Switch, Self::GpioOutput, Self::GpioInput];

    fn apply(self, base_url: &str, name: &str) -> Result<TuiActionMsg> {
        match self {
            Self::Power => perform_control_action(base_url, TIMEOUT, name, false),
            Self::Switch => set_switch_route(base_url, TIMEOUT, name, "target"),
            Self::GpioOutput => set_gpio_output(base_url, TIMEOUT, name, true),
            Self::GpioInput => set_gpio_input(base_url, TIMEOUT, name),
        }
    }

    fn request_path(self) -> &'static str {
        match self {
            Self::Power => "power",
            Self::Switch => "switch",
            Self::GpioOutput | Self::GpioInput => "gpio",
        }
    }

    fn status(self, name: &str) -> String {
        match self {
            Self::Power => format!("power {name}=on"),
            Self::Switch => format!("switch {name}=target"),
            Self::GpioOutput => format!("gpio {name}=1"),
            Self::GpioInput => format!("gpio {name}=input"),
        }
    }

    fn json_body(self) -> &'static str {
        match self {
            Self::Power => r#"{"state":"on"}"#,
            Self::Switch => r#"{"route":"target"}"#,
            Self::GpioOutput => r#"{"direction":"output","value":1}"#,
            Self::GpioInput => r#"{"direction":"input"}"#,
        }
    }
}

fn replies() -> Vec<Reply> {
    (0..VALID_NAMES.len())
        .map(|_| Reply::Http(200, OK))
        .collect()
}

fn request_line(request: &str) -> &str {
    request.lines().next().unwrap()
}

fn no_request_server() -> (String, Receiver<()>, JoinHandle<()>) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let address = listener.local_addr().unwrap();
    listener.set_nonblocking(true).unwrap();
    let (sender, receiver) = mpsc::channel();
    let handle = thread::spawn(move || {
        let deadline = Instant::now() + Duration::from_millis(100);
        loop {
            match listener.accept() {
                Ok(_) => {
                    sender.send(()).unwrap();
                    return;
                }
                Err(error) if error.kind() == ErrorKind::WouldBlock => {
                    if Instant::now() >= deadline {
                        return;
                    }
                    thread::sleep(Duration::from_millis(1));
                }
                Err(error) => panic!("loopback listener failed: {error}"),
            }
        }
    });
    (format!("http://{address}"), receiver, handle)
}

#[test]
fn board_io_operations_encode_hostile_names_as_one_path_segment() {
    for operation in BoardIoOperation::ALL {
        let (base_url, requests) = mock_server(replies());
        for (name, encoded_name) in VALID_NAMES {
            let outcome = operation.apply(&base_url, name).unwrap();

            assert_eq!(outcome.status, operation.status(name));
            assert!(outcome.err.is_none());
            let request = requests.recv().unwrap();
            assert_eq!(
                request_line(&request),
                format!(
                    "PUT /api/v1/{}/{} HTTP/1.1",
                    operation.request_path(),
                    encoded_name
                )
            );
            assert_eq!(
                request.split_once("\r\n\r\n").unwrap().1,
                operation.json_body()
            );
        }
    }
}

#[test]
fn board_io_operations_reject_exact_dot_segments_before_connecting() {
    for operation in BoardIoOperation::ALL {
        for name in [".", ".."] {
            let (base_url, connections, handle) = no_request_server();
            let error = operation.apply(&base_url, name).unwrap_err().to_string();

            assert!(error.contains(name), "unexpected error: {error}");
            assert!(connections.recv_timeout(Duration::from_millis(20)).is_err());
            handle.join().unwrap();
        }
    }
}
