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
    let command = super::tray_command(Path::new("/tmp/linkr-tray"), 18_787, true);

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
            OsStr::new("--headless"),
        ]
    );
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
