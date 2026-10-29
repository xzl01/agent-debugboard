use super::{find_tray_binary, TRAY_TOOL_NAME};
use std::ffi::OsStr;
use std::fs;
use std::path::Path;
use std::process::Command;

#[test]
fn finds_sibling_tray_binary() {
    let dir = std::env::temp_dir().join(format!("linkr-tray-sibling-{}", std::process::id()));
    fs::create_dir_all(&dir).unwrap();
    let path = dir.join(TRAY_TOOL_NAME);
    fs::write(&path, b"").unwrap();
    let old = std::env::var_os("LINKR_TRAY_BIN");
    std::env::set_var("LINKR_TRAY_BIN", &path);
    assert_eq!(find_tray_binary().unwrap(), path);
    if let Some(old) = old {
        std::env::set_var("LINKR_TRAY_BIN", old);
    } else {
        std::env::remove_var("LINKR_TRAY_BIN");
    }
    fs::remove_dir_all(&dir).unwrap();
}

#[cfg(debug_assertions)]
#[test]
fn packaged_tray_candidates_precede_source_tree_fallbacks() {
    let candidates = super::tool_binary_candidates(TRAY_TOOL_NAME, "LINKR_TRAY_BIN");
    let manifest = Path::new(env!("CARGO_MANIFEST_DIR"));
    let release = manifest
        .join("../host-tools/target/release")
        .join(TRAY_TOOL_NAME);
    let debug = manifest
        .join("../host-tools/target/debug")
        .join(TRAY_TOOL_NAME);
    let release_index = candidates
        .iter()
        .rposition(|path| path == &release)
        .unwrap();
    assert_eq!(candidates.last(), Some(&debug));
    assert!(release_index < candidates.len() - 1);

    if let Some(path) = super::find_on_path(TRAY_TOOL_NAME) {
        let path_index = candidates
            .iter()
            .position(|candidate| candidate == &path)
            .unwrap();
        assert!(path_index < release_index);
    }
    if let Ok(exe) = std::env::current_exe() {
        let sibling = exe.parent().unwrap().join(TRAY_TOOL_NAME);
        let sibling_index = candidates.iter().position(|path| path == &sibling).unwrap();
        assert!(sibling_index < release_index);
    }
}

#[test]
fn host_readiness_failure_names_exact_log_path() {
    let log_path = std::path::Path::new("/tmp/radxa-linkr-debugger/host.log");

    let message = super::host::host_not_ready_message(8787, Some(log_path));

    assert!(message.contains(&log_path.display().to_string()));
}

#[test]
fn desktop_log_path_uses_ascii_slug() {
    assert_eq!(super::APP_DATA_DIR_NAME, "radxa-linkr-debugger");
}

#[test]
fn headless_linux_skips_tray() {
    assert!(!super::graphical_session_available_from(None, None));
    assert!(!super::graphical_session_available_from(
        Some(OsStr::new("")),
        Some(OsStr::new(""))
    ));
    assert!(super::graphical_session_available_from(
        Some(OsStr::new(":0")),
        None
    ));
    assert!(super::graphical_session_available_from(
        None,
        Some(OsStr::new("wayland-0"))
    ));
}

#[test]
fn graphical_launch_can_promote_a_healthy_headless_daemon() {
    assert!(super::should_launch_tray(true, true));
    assert!(super::should_launch_tray(false, true));
    assert!(super::should_launch_tray(false, false));
    assert!(!super::should_launch_tray(true, false));
}

#[test]
fn tray_readiness_requires_the_matching_graphical_lock_owner() {
    assert!(super::host::tray_lock_allows_ready("42 tray\n", 42, true));
    assert!(!super::host::tray_lock_allows_ready(
        "42 headless\n",
        42,
        true
    ));
    assert!(!super::host::tray_lock_allows_ready("41 tray\n", 42, true));
    assert!(super::host::tray_lock_allows_ready("41 tray\n", 42, false));
    assert!(!super::host::tray_lock_allows_ready(
        "41 headless\n",
        42,
        false
    ));
    assert!(!super::host::tray_lock_allows_ready("42 tray\n", 42, false));
    assert!(!super::host::tray_lock_allows_ready(
        "41 tray garbage\n",
        42,
        false
    ));
    assert!(!super::host::tray_lock_allows_ready("+42 tray\n", 42, true));
    assert!(!super::host::tray_lock_allows_ready("42\ttray\n", 42, true));
    assert!(!super::host::tray_lock_allows_ready("42 tray", 42, true));
    assert!(super::host::tray_lock_owner_is_headless(
        "7 headless\n",
        Some(7)
    ));
    assert!(!super::host::tray_lock_owner_is_headless(
        "6 headless\n",
        Some(7)
    ));

    let replacing_headless = super::HostLaunch {
        port: 0,
        board_url: "http://192.0.2.1",
        previous_host_pid: Some(7),
        launcher_name: "linkr-tray",
        log_path: None,
        tray_lock: None,
        replacing_headless: true,
    };
    assert!(!replacing_headless.accepts_host(7, true));
    assert!(replacing_headless.accepts_host(8, false));
    let adopting_standalone = super::HostLaunch {
        replacing_headless: false,
        ..replacing_headless
    };
    assert!(adopting_standalone.accepts_host(7, true));
}

#[test]
fn headless_launch_uses_the_tray_as_the_single_daemon() {
    let command = super::tray_command(
        Path::new("/tmp/linkr-tray"),
        18_787,
        "http://10.123.5.149",
        true,
    );

    #[cfg(target_os = "linux")]
    assert_eq!(command.get_program(), OsStr::new("setsid"));
    #[cfg(not(target_os = "linux"))]
    assert_eq!(command.get_program(), OsStr::new("/tmp/linkr-tray"));
    assert_eq!(
        command.get_args().collect::<Vec<_>>(),
        [
            #[cfg(target_os = "linux")]
            OsStr::new("/tmp/linkr-tray"),
            OsStr::new("--no-open-when-running"),
            OsStr::new("--port"),
            OsStr::new("18787"),
            OsStr::new("--board-url"),
            OsStr::new("http://10.123.5.149"),
            OsStr::new("--headless"),
        ]
    );
}

#[cfg(unix)]
#[test]
fn detached_launcher_passes_the_resolved_board_url_to_a_fake_tray() {
    use std::os::unix::fs::PermissionsExt;

    let dir = std::env::temp_dir().join(format!("linkr-tray-args-{}", std::process::id()));
    fs::create_dir_all(&dir).unwrap();
    let tray = dir.join("fake-tray");
    let captured = dir.join("args");
    fs::write(
        &tray,
        "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$LINKR_TEST_TRAY_ARGS\"\n",
    )
    .unwrap();
    let mut permissions = fs::metadata(&tray).unwrap().permissions();
    permissions.set_mode(0o700);
    fs::set_permissions(&tray, permissions).unwrap();

    let mut command = super::tray_command(&tray, 18_787, "http://10.123.5.149", true);
    command.env("LINKR_TEST_TRAY_ARGS", &captured);
    let mut child = super::spawn_detached(command, None).unwrap();
    assert!(child.wait().unwrap().success());
    for _ in 0..100 {
        if captured.is_file() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(10));
    }

    assert_eq!(
        fs::read_to_string(&captured).unwrap(),
        "--no-open-when-running\n--port\n18787\n--board-url\nhttp://10.123.5.149\n--headless\n"
    );
    fs::remove_dir_all(dir).unwrap();
}

fn one_shot_host_status(board_url: &str) -> (u16, std::thread::JoinHandle<()>) {
    let body = serde_json::json!({
        "schema": "radxa-linkr-host.v1",
        "ok": true,
        "pid": 42,
        "board_url": board_url,
    })
    .to_string();
    one_shot_host_response(body)
}

fn one_shot_host_response(body: String) -> (u16, std::thread::JoinHandle<()>) {
    use std::io::{Read, Write};

    let listener = std::net::TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0)).unwrap();
    let port = listener.local_addr().unwrap().port();
    let server = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        stream
            .set_read_timeout(Some(std::time::Duration::from_secs(2)))
            .unwrap();
        let mut request = Vec::new();
        loop {
            let mut chunk = [0_u8; 1024];
            let count = stream.read(&mut chunk).unwrap();
            assert_ne!(count, 0, "client closed before sending Host status request");
            request.extend_from_slice(&chunk[..count]);
            if request.windows(4).any(|window| window == b"\r\n\r\n") {
                break;
            }
        }
        assert!(String::from_utf8(request)
            .unwrap()
            .starts_with("GET /host/api/v1/status "));
        write!(
            stream,
            "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        )
        .unwrap();
    });
    (port, server)
}

#[test]
fn running_host_with_the_same_board_url_is_reused_without_launching_a_tray() {
    let (port, server) = one_shot_host_status("http://10.123.5.149/");

    super::ensure_tray_daemon_with_launcher(port, "http://10.123.5.149", || {
        panic!("matching Host must not resolve or launch Tray")
    })
    .unwrap();

    server.join().unwrap();
}

#[test]
fn running_host_with_a_different_board_url_gets_a_restart_diagnostic() {
    let (port, server) = one_shot_host_status("http://172.29.203.1/");

    let error = super::ensure_tray_daemon_with_launcher(port, "http://10.123.5.149", || {
        panic!("mismatching Host must not resolve or launch Tray")
    })
    .unwrap_err();

    server.join().unwrap();
    let message = error.to_string();
    assert!(message.contains("http://172.29.203.1/"), "{message}");
    assert!(message.contains("http://10.123.5.149"), "{message}");
    assert!(
        message.contains("was not stopped or retargeted"),
        "{message}"
    );
    assert!(
        message.contains("quit the existing Radxa Linkr Tray"),
        "{message}"
    );
    assert!(message.contains("--desktop"), "{message}");
}

#[test]
fn failed_host_status_fixture_only_calls_the_injected_fake_launcher() {
    for response in [
        "not JSON".to_string(),
        serde_json::json!({"schema":"unrelated-service", "ok":true, "pid":42, "board_url":"http://192.0.2.1"}).to_string(),
        serde_json::json!({"schema":"radxa-linkr-host.v1", "ok":false}).to_string(),
    ] {
        let (port, server) = one_shot_host_response(response);
        let launches = std::cell::Cell::new(0);
        let error = super::ensure_tray_daemon_with_launcher(port, "http://192.0.2.1", || {
            launches.set(launches.get() + 1);
            anyhow::bail!("fake resolver/launcher refused startup")
        }).unwrap_err();
        server.join().unwrap();
        assert_eq!(launches.get(), 1);
        assert_eq!(error.to_string(), "fake resolver/launcher refused startup");
    }
}

#[test]
fn desktop_launcher_failure_is_logged_and_reported() {
    let dir = std::env::temp_dir().join(format!("linkr-desktop-{}", std::process::id()));
    fs::create_dir_all(&dir).unwrap();
    let log_path = dir.join("host.log");
    let mut command = Command::new(std::env::current_exe().unwrap());
    command
        .args([
            "--exact",
            "desktop::tests::desktop_launcher_test_child",
            "--nocapture",
        ])
        .env("LINKR_DESKTOP_TEST_CHILD", "1");
    super::configure_desktop_output(&mut command, Some(&log_path)).unwrap();
    let mut child = command.spawn().unwrap();

    let error = super::wait_for_host_ready(
        super::HostLaunch {
            port: 0,
            board_url: "http://192.0.2.1",
            previous_host_pid: None,
            launcher_name: "linkr-tray",
            log_path: Some(&log_path),
            tray_lock: None,
            replacing_headless: false,
        },
        &mut child,
    )
    .unwrap_err()
    .to_string();

    assert!(error.contains("linkr-tray exited"), "{error}");
    assert!(error.contains(&log_path.display().to_string()), "{error}");
    assert!(fs::read_to_string(&log_path)
        .unwrap()
        .contains("desktop launcher test stderr"));
    fs::remove_dir_all(dir).unwrap();
}

#[test]
fn desktop_launcher_test_child() {
    if std::env::var_os("LINKR_DESKTOP_TEST_CHILD").is_some() {
        eprintln!("desktop launcher test stderr");
        std::process::exit(23);
    }
}
