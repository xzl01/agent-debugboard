use super::*;
use crate::discovery_test_support::{boards, status, NoDiscovery};
use std::cell::RefCell;

struct TestClient(BoardTarget);

impl BoardTransport for TestClient {
    fn base_url(&self) -> &str {
        &self.0.base_url
    }
    fn target(&self) -> BoardTarget {
        self.0.clone()
    }
    fn send_text(&self, request: BoardRequest) -> Result<String> {
        assert_eq!(request.method, Method::GET);
        assert_eq!(request.path, "/api/v1/status");
        Ok(status())
    }
    fn upload_binary(&self, _: BoardBinaryUpload) -> Result<String> {
        panic!("unexpected upload")
    }
}

#[derive(Default)]
struct TestDesktop {
    targets: RefCell<Vec<BoardTarget>>,
    error: Option<String>,
}

impl DesktopLauncher for TestDesktop {
    fn ensure(&self, target: &BoardTarget) -> Result<()> {
        self.targets.borrow_mut().push(target.clone());
        if let Some(error) = &self.error {
            anyhow::bail!(error.clone());
        }
        Ok(())
    }
}

#[derive(Default)]
struct TestTui(RefCell<Option<BoardTarget>>);

impl TuiRunner for TestTui {
    fn run_tui(
        &self,
        target: &BoardTarget,
        _: Duration,
        _: &mut dyn Write,
        _: &mut dyn Write,
    ) -> Result<u8> {
        *self.0.borrow_mut() = Some(target.clone());
        Ok(0)
    }
}

struct Outcome {
    code: u8,
    stdout: String,
    stderr: String,
    connections: Vec<String>,
    tui_url: Option<String>,
    targets: Vec<BoardTarget>,
    tui_target: Option<BoardTarget>,
    desktop_targets: Vec<BoardTarget>,
}

fn invoke(args: &[&str], env_url: Option<&str>, backend: &impl DiscoveryBackend) -> Outcome {
    invoke_with_desktop(args, env_url, backend, &TestDesktop::default())
}

fn invoke_with_desktop(
    args: &[&str],
    env_url: Option<&str>,
    backend: &impl DiscoveryBackend,
    desktop: &TestDesktop,
) -> Outcome {
    let cli = Cli::parse_from(std::iter::once("cmd").chain(args.iter().copied()));
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();
    let mut connections = Vec::new();
    let mut targets = Vec::new();
    let tui = TestTui::default();
    let code = run_cli_with_io(
        cli,
        env_url,
        backend,
        desktop,
        |target, _| {
            connections.push(target.base_url.clone());
            targets.push(target.clone());
            Ok(TestClient(target.clone()))
        },
        &tui,
        (&mut stdout, &mut stderr),
    )
    .unwrap();
    let tui_target = tui.0.into_inner();
    Outcome {
        code,
        stdout: String::from_utf8(stdout).unwrap(),
        stderr: String::from_utf8(stderr).unwrap(),
        connections,
        tui_url: tui_target.as_ref().map(|target| target.base_url.clone()),
        targets,
        tui_target,
        desktop_targets: std::mem::take(&mut *desktop.targets.borrow_mut()),
    }
}

#[test]
fn discover_json_lists_every_verified_device_with_metadata_and_timeout() {
    for args in [
        vec!["--json", "--timeout", "45ms", "discover"],
        vec!["discover", "--json", "--timeout", "45ms"],
        vec!["discover", "--timeout=45ms", "--json"],
    ] {
        let backend = boards(3);
        let result = invoke(&args, None, &backend);
        assert_eq!(result.code, 0);
        assert!(result.connections.is_empty());
        assert!(result.desktop_targets.is_empty());
        assert!(result.tui_url.is_none());
        assert!(result.stderr.is_empty());
        let output: Value = serde_json::from_str(&result.stdout).unwrap();
        assert_eq!(output["schema"], JSON_SCHEMA);
        assert_eq!(output["ok"], true);
        assert_eq!(output["command"], "discover");
        assert_eq!(output["devices"].as_array().unwrap().len(), 3);
        let device = &output["devices"][0];
        assert_eq!(device["base_url"], "http://192.0.2.1");
        assert_eq!(device["interface"], "test-0");
        assert_eq!(device["local_ip"], "192.0.2.2");
        assert_eq!(device["project"], "radxa-linkr-debugger");
        assert_eq!(device["mcu"], "test-mcu");
        assert_eq!(device["usb"], json!({"product": "test-product"}));
        assert_eq!(device["image_version"], "test-version");
        assert!(backend
            .probes
            .lock()
            .unwrap()
            .iter()
            .all(|(_, timeout)| *timeout == Duration::from_millis(45)));
    }
}

#[test]
fn discover_json_omits_rejected_services_and_reports_each_failure_kind() {
    let mut backend = boards(3);
    backend.responses.insert(
        "http://192.0.2.1".into(),
        Err(JsonError {
            code: "transport_error".into(),
            message: "timed out".into(),
        }),
    );
    backend.responses.insert(
        "http://192.0.2.5".into(),
        Ok(json!({
            "schema": JSON_SCHEMA, "ok": true, "command": "status", "project": "other-service",
        })
        .to_string()),
    );
    let result = invoke(&["discover", "--json"], None, &backend);
    assert_eq!(result.code, 0);
    let output: Value = serde_json::from_str(&result.stdout).unwrap();
    assert_eq!(output["devices"].as_array().unwrap().len(), 1);
    assert_eq!(output["devices"][0]["base_url"], "http://192.0.2.9");
    assert_eq!(output["failures"][0]["error"]["code"], "transport_error");
    assert_eq!(output["failures"][1]["error"]["code"], "validation_failed");
}

#[test]
fn discover_empty_is_a_successful_empty_list_and_text_lists_all_boards() {
    let result = invoke(&["discover", "--json"], None, &boards(0));
    assert_eq!(result.code, 0);
    let output: Value = serde_json::from_str(&result.stdout).unwrap();
    assert_eq!(output["ok"], true);
    assert_eq!(output["devices"], json!([]));
    let result = invoke(&["discover"], None, &boards(2));
    assert!(result
        .stdout
        .contains("http://192.0.2.1 interface=test-0 local_ip=192.0.2.2"));
    assert!(result
        .stdout
        .contains("http://192.0.2.5 interface=test-1 local_ip=192.0.2.6"));
}

#[test]
fn discover_rejects_bad_arguments_without_network_access() {
    for (args, env) in [
        (vec!["discover", "extra", "--json"], None),
        (vec!["discover", "--json", "--timeout"], None),
        (vec!["discover", "--json", "--timeout=0"], None),
        (vec!["discover", "--json", "--timeout=NaN"], None),
    ] {
        let result = invoke(&args, env, &NoDiscovery);
        assert_eq!(result.code, 2);
        let output: Value = serde_json::from_str(&result.stdout).unwrap();
        assert_eq!(output["command"], "discover");
        assert_eq!(output["error"]["code"], "usage");
        assert!(result.connections.is_empty());
    }
}

#[test]
fn explicit_discover_always_scans_even_with_configured_targets() {
    use std::sync::atomic::Ordering;
    for (args, env) in [
        (
            vec!["--url", "board.test", "discover", "--json"],
            Some("env.test"),
        ),
        (vec!["--addr", "board.test", "discover", "--json"], None),
        (vec!["--port", "board.test", "discover", "--json"], None),
        (vec!["discover", "--json"], Some("env.test")),
        (vec!["discover", "--json"], Some("")),
    ] {
        let backend = boards(2);
        let result = invoke(&args, env, &backend);
        assert_eq!(result.code, 0);
        assert_eq!(backend.enumerations.load(Ordering::SeqCst), 1);
        assert_eq!(backend.probes.lock().unwrap().len(), 2);
        assert!(result.connections.is_empty());
        let output: Value = serde_json::from_str(&result.stdout).unwrap();
        assert_eq!(output["devices"].as_array().unwrap().len(), 2);
    }
}

#[test]
fn zero_and_multiple_devices_fail_before_constructing_any_control_client() {
    for count in [0, 2] {
        let result = invoke(
            &["--json", "power", "set", "arbitrary-output", "on"],
            None,
            &boards(count),
        );
        assert_eq!(result.code, 1);
        assert!(result.connections.is_empty());
        assert!(result.desktop_targets.is_empty());
        assert!(result.tui_url.is_none());
        let output: Value = serde_json::from_str(&result.stdout).unwrap();
        assert_eq!(output["schema"], JSON_SCHEMA);
        assert_eq!(output["command"], "power");
        assert_eq!(output["ok"], false);
        assert_eq!(
            output["error"]["code"],
            if count == 0 {
                "device_not_found"
            } else {
                "multiple_devices"
            }
        );
        assert_eq!(output["devices"].as_array().unwrap().len(), count);
        assert!(output["error"]["message"]
            .as_str()
            .unwrap()
            .contains("--url"));
    }
    let result = invoke(&["status"], None, &boards(2));
    assert!(result.stderr.contains("multiple_devices"));
    assert!(result.stderr.contains("http://192.0.2.1"));
    assert!(result.stderr.contains("http://192.0.2.5"));
}

#[test]
fn resolved_url_reaches_doctor_and_tui_without_second_resolution() {
    let result = invoke(&["--json", "doctor"], None, &boards(1));
    assert_eq!(result.code, 0);
    assert_eq!(result.connections, ["http://192.0.2.1"]);
    assert_eq!(result.desktop_targets, result.targets);
    assert_eq!(result.desktop_targets.len(), 1);
    assert_eq!(
        result.targets[0].local_ip,
        Some("192.0.2.2".parse().unwrap())
    );
    let output: Value = serde_json::from_str(&result.stdout).unwrap();
    assert_eq!(output["base_url"], "http://192.0.2.1");
    assert_eq!(output["probe_ok"], true);
    let result = invoke(&[], None, &boards(1));
    assert_eq!(result.code, 0);
    assert_eq!(result.tui_url.as_deref(), Some("http://192.0.2.1"));
    assert_eq!(result.desktop_targets.len(), 1);
    assert_eq!(result.desktop_targets[0].base_url, "http://192.0.2.1");
    assert_eq!(
        result.tui_target.unwrap().local_ip,
        Some("192.0.2.2".parse().unwrap())
    );
}

#[test]
fn explicit_and_env_urls_skip_enumeration_in_the_startup_path() {
    for (args, env, expected) in [
        (
            vec!["--url", "chosen.test/", "--json", "doctor"],
            Some("ignored.test"),
            "http://chosen.test",
        ),
        (vec![], Some("env.test/"), "http://env.test"),
    ] {
        let result = invoke(&args, env, &NoDiscovery);
        assert_eq!(result.code, 0);
        assert_eq!(result.connections, [expected]);
        assert!(result.targets[0].local_ip.is_none());
    }
    for args in [vec!["--version"], vec!["--json"], vec!["--raw", "status"]] {
        let result = invoke(&args, None, &NoDiscovery);
        assert!(result.connections.is_empty());
        assert!(result.desktop_targets.is_empty());
    }
}

#[test]
fn help_and_clap_errors_do_not_start_the_desktop() {
    for (args, expected_code) in [
        (vec!["cmd", "--help"], 0),
        (vec!["cmd", "--timeout", "not-a-duration"], 2),
    ] {
        let desktop = TestDesktop::default();
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();
        let code = run_with_io_and_desktop(args, &mut stdout, &mut stderr, &desktop).unwrap();
        assert_eq!(code, expected_code);
        assert!(desktop.targets.borrow().is_empty());
    }
}

#[test]
fn invalid_commands_finish_without_starting_the_desktop() {
    for args in [
        vec!["not-a-command"],
        vec!["status", "extra"],
        vec!["adc", "record", "out.ndjson", "--rate-hz", "251"],
        vec!["test", "run"],
        vec!["gpio", "set", "GP13", "typo"],
        vec!["power", "set", "arbitrary-output", "typo"],
        vec!["doctor", "extra"],
        vec!["reboot", "extra"],
        vec!["task", "bogus"],
        vec!["task", "delete", "invalid task id"],
        vec!["task", "run", "invalid task id", "--confirm"],
        vec!["task", "source", "invalid task id", "unused.ndjson"],
        vec!["config", "save"],
        vec!["ip", "mac", "not-a-mac"],
        vec!["pd", "fixed", "typo"],
        vec!["ota", "upload"],
    ] {
        for json_output in [false, true] {
            let mut invocation = args.clone();
            if json_output {
                invocation.insert(0, "--json");
            }
            let result = invoke(&invocation, None, &NoDiscovery);
            assert_eq!(result.code, 2, "args={invocation:?}");
            assert!(result.connections.is_empty(), "args={invocation:?}");
            assert!(result.desktop_targets.is_empty(), "args={invocation:?}");
            assert!(result.tui_target.is_none());
            if json_output {
                let envelope: Value = serde_json::from_str(&result.stdout).unwrap();
                assert_eq!(envelope["schema"], JSON_SCHEMA);
                assert_eq!(envelope["ok"], false);
                assert_eq!(envelope["error"]["code"], "usage");
                assert!(result.stderr.is_empty());
            } else {
                assert!(result.stdout.is_empty());
                assert!(!result.stderr.is_empty());
            }
        }
    }
}

#[test]
fn local_files_are_validated_before_discovery_and_explicit_desktop_startup() {
    let dir = std::env::temp_dir().join(format!("linkr-local-prepare-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let malformed = dir.join("malformed.ndjson");
    std::fs::write(&malformed, b"invalid test script\n").unwrap();
    let empty = dir.join("empty.bin");
    std::fs::write(&empty, b"").unwrap();
    let missing = dir.join("missing.bin");
    for desktop in [false, true] {
        for (args, expected_error) in [
            (
                vec!["ota", "upload", missing.to_str().unwrap()],
                "invalid_file",
            ),
            (
                vec!["ota", "upload", empty.to_str().unwrap()],
                "invalid_file",
            ),
            (
                vec!["test", "run", missing.to_str().unwrap()],
                "script_open_failed",
            ),
            (
                vec!["test", "run", malformed.to_str().unwrap()],
                "parse_error",
            ),
            (
                vec!["task", "store", missing.to_str().unwrap()],
                "read_failed",
            ),
            (
                vec!["task", "import", missing.to_str().unwrap()],
                "read_failed",
            ),
        ] {
            let mut invocation = vec!["--json"];
            if desktop {
                invocation.push("--desktop");
            }
            invocation.extend(args);
            let result = invoke(&invocation, None, &NoDiscovery);
            assert_eq!(result.code, 2, "args={invocation:?}");
            let envelope: Value = serde_json::from_str(&result.stdout).unwrap();
            assert_eq!(envelope["error"]["code"], expected_error);
            assert!(result.stderr.is_empty());
            assert!(result.connections.is_empty());
            assert!(result.desktop_targets.is_empty());
        }
    }
    std::fs::remove_dir_all(dir).unwrap();
}

#[test]
fn task_help_is_purely_local() {
    let result = invoke(&["task", "--help"], None, &NoDiscovery);
    assert_eq!(result.code, 0);
    assert!(result.stdout.contains("usage:"));
    assert!(result.connections.is_empty());
    assert!(result.desktop_targets.is_empty());
}

#[test]
fn implicit_desktop_failure_warns_and_the_board_command_continues() {
    let desktop = TestDesktop {
        targets: RefCell::new(Vec::new()),
        error: Some("test desktop unavailable".into()),
    };
    let result = invoke_with_desktop(&["status"], None, &boards(1), &desktop);

    assert_eq!(result.code, 0);
    assert_eq!(result.connections, ["http://192.0.2.1"]);
    assert!(result.stdout.contains("\"command\":\"status\""));
    assert!(result.stderr.contains("test desktop unavailable"));
    assert!(result.stderr.contains("continuing with the board command"));
}

#[test]
fn explicit_desktop_uses_the_resolved_target_without_running_a_board_command() {
    for args in [
        vec!["--desktop", "status"],
        vec!["--desktop"],
        vec!["--json", "--desktop"],
        vec!["--desktop", "--json"],
    ] {
        let result = invoke(&args, None, &boards(1));

        assert_eq!(result.code, 0, "args={args:?}");
        assert!(result.connections.is_empty());
        assert_eq!(result.desktop_targets.len(), 1);
        assert_eq!(result.desktop_targets[0].base_url, "http://192.0.2.1");
        assert_eq!(
            result.desktop_targets[0].local_ip,
            Some("192.0.2.2".parse().unwrap())
        );
        assert!(result.stdout.is_empty());
        assert!(result.stderr.is_empty());
    }
}

#[test]
fn explicit_desktop_failure_is_fatal_before_constructing_a_control_client() {
    let cli = Cli::parse_from(["cmd", "--desktop"]);
    let desktop = TestDesktop {
        targets: RefCell::new(Vec::new()),
        error: Some("explicit desktop failed".into()),
    };
    let tui = TestTui::default();
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();

    let error = run_cli_with_io(
        cli,
        None,
        &boards(1),
        &desktop,
        |_, _| -> Result<TestClient> {
            panic!("desktop failure must prevent control-client construction")
        },
        &tui,
        (&mut stdout, &mut stderr),
    )
    .unwrap_err();

    assert!(error.to_string().contains("explicit desktop failed"));
    assert!(stderr.is_empty());
}
