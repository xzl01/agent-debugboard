use std::{
    fs,
    io::{Read, Write},
    net::{TcpListener, TcpStream},
    path::Path,
    process::{Child, Command, Stdio},
    thread,
    time::{Duration, Instant},
};

use serde_json::Value;
use tempfile::TempDir;

struct ChildGuard(Child);

impl Drop for ChildGuard {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

fn unused_port() -> u16 {
    TcpListener::bind(("127.0.0.1", 0))
        .expect("bind ephemeral port")
        .local_addr()
        .expect("read ephemeral port")
        .port()
}

fn request_status(port: u16) -> Option<Value> {
    let mut stream = TcpStream::connect(("127.0.0.1", port)).ok()?;
    stream
        .set_read_timeout(Some(Duration::from_millis(200)))
        .ok()?;
    stream
        .write_all(
            format!(
                "GET /host/api/v1/status HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nConnection: close\r\n\r\n"
            )
            .as_bytes(),
        )
        .ok()?;
    let mut response = String::new();
    stream.read_to_string(&mut response).ok()?;
    let (_, body) = response.split_once("\r\n\r\n")?;
    serde_json::from_str(body).ok()
}

fn wait_for_status(port: u16, child: &mut Child) -> Value {
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        if let Some(status) = request_status(port) {
            return status;
        }
        assert!(
            child.try_wait().expect("inspect daemon").is_none(),
            "desktop daemon exited before Host became ready"
        );
        assert!(Instant::now() < deadline, "Host did not become ready");
        thread::sleep(Duration::from_millis(25));
    }
}

fn wait_for_exit(child: &mut Child) {
    let deadline = Instant::now() + Duration::from_secs(2);
    while child.try_wait().expect("inspect second daemon").is_none() {
        assert!(Instant::now() < deadline, "second daemon did not exit");
        thread::sleep(Duration::from_millis(10));
    }
}

#[test]
fn headless_daemon_is_singleton_across_launch_environments_and_hosts_in_process() {
    let root = TempDir::new().expect("create test root");
    let data_home = root.path().join("data");
    let web_root = root.path().join("web");
    fs::create_dir_all(&web_root).expect("create Web root");
    fs::write(web_root.join("index.html"), "linkr").expect("write Web index");
    let port = unused_port();
    let binary = Path::new(env!("CARGO_BIN_EXE_linkr-tray"));
    let spawn = |inherit_library_path: bool| {
        let mut command = Command::new(binary);
        command
            .arg("--headless")
            .arg("--port")
            .arg(port.to_string())
            .arg("--web-root")
            .arg(&web_root)
            .env("XDG_DATA_HOME", &data_home)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        if !inherit_library_path {
            command.env_remove("LD_LIBRARY_PATH");
        }
        command.spawn().expect("start desktop daemon")
    };
    let mut first = ChildGuard(spawn(true));

    let status = wait_for_status(port, &mut first.0);

    assert_eq!(
        status.get("pid").and_then(Value::as_u64),
        Some(u64::from(first.0.id()))
    );
    let lock = fs::read_to_string(data_home.join("radxa-linkr-debugger/tray.lock"))
        .expect("read daemon lock");
    assert_eq!(lock.split_whitespace().nth(1), Some("headless"));
    let mut second = spawn(false);
    wait_for_exit(&mut second);
    assert_eq!(
        request_status(port).and_then(|value| value.get("pid").and_then(Value::as_u64)),
        Some(u64::from(first.0.id()))
    );

    let show_tray_request = data_home
        .join("radxa-linkr-debugger")
        .join("show-tray.request");
    fs::write(show_tray_request, []).expect("request graphical tray");
    wait_for_exit(&mut first.0);
}

#[test]
fn headless_tray_adopts_a_compatible_standalone_host() {
    let root = TempDir::new().expect("create test root");
    let data_home = root.path().join("data");
    let web_root = root.path().join("web");
    fs::create_dir_all(&web_root).expect("create Web root");
    fs::write(web_root.join("index.html"), "linkr").expect("write Web index");
    let port = unused_port();
    let mut host = ChildGuard(
        Command::new(Path::new(env!("CARGO_BIN_EXE_linkr-host")))
            .arg("serve")
            .arg("--port")
            .arg(port.to_string())
            .arg("--web-root")
            .arg(&web_root)
            .env("XDG_DATA_HOME", &data_home)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("start standalone Host"),
    );
    let status = wait_for_status(port, &mut host.0);
    assert_eq!(
        status.get("pid").and_then(Value::as_u64),
        Some(u64::from(host.0.id()))
    );

    let mut tray = Command::new(Path::new(env!("CARGO_BIN_EXE_linkr-tray")))
        .arg("--headless")
        .arg("--port")
        .arg(port.to_string())
        .arg("--web-root")
        .arg(&web_root)
        .env("XDG_DATA_HOME", &data_home)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .expect("start headless tray");
    wait_for_exit(&mut tray);

    assert!(tray
        .try_wait()
        .expect("inspect headless tray")
        .expect("headless tray exited")
        .success());
    assert_eq!(
        request_status(port).and_then(|value| value.get("pid").and_then(Value::as_u64)),
        Some(u64::from(host.0.id()))
    );
}
