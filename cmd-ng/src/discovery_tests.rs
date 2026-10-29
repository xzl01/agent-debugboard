use super::*;
use crate::discovery_test_support::{boards, interface, status, FakeDiscovery, NoDiscovery};
use clap::Parser;
use serde_json::json;

#[test]
fn only_active_usable_slash30_peers_are_candidates() {
    let mut down = interface("down", "192.0.2.10");
    down.up = false;
    let mut wide = interface("wide", "192.0.2.14");
    wide.netmask = "255.255.255.0".parse().unwrap();
    wide.prefixlen = 24;
    let mut narrow = interface("narrow", "192.0.2.18");
    narrow.netmask = "255.255.255.254".parse().unwrap();
    narrow.prefixlen = 31;
    let interfaces = vec![
        interface("arbitrary-a", "192.0.2.2"),
        interface("arbitrary-b", "192.0.2.5"),
        interface("arbitrary-a", "192.0.2.2"),
        down,
        wide,
        narrow,
        interface("network", "192.0.2.20"),
        interface("broadcast", "192.0.2.27"),
        interface("loopback", "127.0.0.2"),
        interface("multicast", "224.0.0.2"),
        interface("unspecified", "0.0.0.0"),
    ];
    let found = candidates(interfaces);
    assert_eq!(found.len(), 2);
    assert_eq!(found[0].base_url, "http://192.0.2.1");
    assert_eq!(found[1].base_url, "http://192.0.2.6");
    assert_eq!(found[1].local_ip, "192.0.2.5".parse::<Ipv4Addr>().unwrap());
}

#[test]
fn slash30_prefix_is_authoritative_when_windows_reports_a_host_netmask() {
    let mut iface = interface("windows-ncm", "192.0.2.2");
    iface.netmask = "255.255.255.255".parse().unwrap();
    let candidate = candidates(vec![iface.clone()]).remove(0);
    assert_eq!(candidate.base_url, "http://192.0.2.1");
    assert_eq!(candidate.prefixlen, 30);
    assert_eq!(candidate.netmask, iface.netmask);

    iface.prefixlen = 32;
    iface.netmask = "255.255.255.252".parse().unwrap();
    assert!(candidates(vec![iface]).is_empty());
}

#[test]
fn never_probes_a_local_address_even_on_a_different_interface() {
    let mut other = interface("other", "192.0.2.1");
    other.netmask = "255.255.255.0".parse().unwrap();
    other.prefixlen = 24;
    other.up = false;
    assert!(candidates(vec![interface("ncm", "192.0.2.2"), other]).is_empty());
}

#[test]
fn validates_every_identity_field_strictly_and_preserves_metadata() {
    let candidate = candidates(vec![interface("test", "192.0.2.2")]).remove(0);
    let valid: Value = serde_json::from_str(&status()).unwrap();
    let device = validate_status(candidate.clone(), &valid.to_string()).unwrap();
    assert_eq!(device.mcu, Some(json!("test-mcu")));
    assert_eq!(device.usb, Some(json!({"product": "test-product"})));
    assert_eq!(device.image_version, Some(json!("test-version")));
    for (field, wrong) in [
        ("schema", json!("other.v1")),
        ("ok", json!(false)),
        ("ok", json!("true")),
        ("ok", json!(1)),
        ("command", json!("power")),
        ("project", json!("unrelated-service")),
    ] {
        let mut value = valid.clone();
        value[field] = wrong;
        assert_eq!(
            validate_status(candidate.clone(), &value.to_string())
                .unwrap_err()
                .code,
            "validation_failed"
        );
        value.as_object_mut().unwrap().remove(field);
        assert!(validate_status(candidate.clone(), &value.to_string()).is_err());
    }
    for body in ["", "not JSON", "[]", "{}", "null"] {
        assert!(validate_status(candidate.clone(), body).is_err());
    }
    let minimal = json!({"schema": JSON_SCHEMA, "ok": true, "command": "status", "project": "radxa-linkr-debugger"});
    let minimal = validate_status(candidate, &minimal.to_string()).unwrap();
    assert!(minimal.mcu.is_none() && minimal.usb.is_none() && minimal.image_version.is_none());
}

#[test]
fn discovers_all_candidates_across_batches_and_keeps_transport_and_validation_failures() {
    let mut backend = boards(11);
    backend
        .responses
        .insert("http://192.0.2.1".into(), Err(transport_error("timeout")));
    backend
        .responses
        .insert("http://192.0.2.5".into(), Ok("{}".into()));
    let timeout = Duration::from_millis(35);
    let report = discover(&backend, timeout).unwrap();
    assert_eq!(report.devices.len(), 9);
    assert_eq!(report.failures.len(), 2);
    assert_eq!(report.failures[0].error.code, "transport_error");
    assert_eq!(report.failures[1].error.code, "validation_failed");
    let calls = backend.probes.lock().unwrap();
    assert_eq!(calls.len(), 11);
    assert!(calls.iter().all(|(_, seen)| *seen == timeout));
}

#[test]
fn explicit_flags_and_environment_short_circuit_all_discovery() {
    for (flags, env, expected) in [
        (
            vec![
                "--url",
                "https://url.test/",
                "--addr",
                "addr.test",
                "--port",
                "port.test",
            ],
            Some("env.test"),
            "https://url.test",
        ),
        (
            vec!["--addr", "addr.test/", "--port", "port.test"],
            Some("env.test"),
            "http://addr.test",
        ),
        (
            vec!["--port", "port.test/"],
            Some("env.test"),
            "http://port.test",
        ),
        (vec![], Some(" env.test/prefix/ "), "http://env.test/prefix"),
    ] {
        let cli = Cli::parse_from(std::iter::once("cmd").chain(flags));
        let target = resolve_target(&cli, env, &NoDiscovery).unwrap();
        assert_eq!(target.base_url, expected);
        assert!(target.local_ip.is_none());
    }
    for flags in [
        vec!["--url", ""],
        vec!["--addr", " "],
        vec!["--port", "http://"],
    ] {
        let cli = Cli::parse_from(std::iter::once("cmd").chain(flags));
        assert_eq!(
            resolve_target(&cli, Some("env.test"), &NoDiscovery)
                .unwrap_err()
                .error
                .code,
            "invalid_url"
        );
    }
    let cli = Cli::parse_from(["cmd"]);
    for env in ["", " ", "http://", "ftp://invalid.test"] {
        assert_eq!(
            resolve_target(&cli, Some(env), &NoDiscovery)
                .unwrap_err()
                .error
                .code,
            "invalid_url"
        );
    }
}

#[test]
fn auto_selection_requires_exactly_one_verified_device() {
    let cli = Cli::parse_from(["cmd", "--timeout", "25ms", "status"]);
    let empty = resolve_target(&cli, None, &FakeDiscovery::default()).unwrap_err();
    assert_eq!(empty.error.code, "device_not_found");
    assert!(empty.report.devices.is_empty());
    assert_eq!(
        resolve_target(&cli, None, &boards(1)).unwrap().base_url,
        "http://192.0.2.1"
    );
    let multiple = resolve_target(&cli, None, &boards(2)).unwrap_err();
    assert_eq!(multiple.error.code, "multiple_devices");
    assert_eq!(multiple.report.devices.len(), 2);
    assert!(multiple.error.message.contains("--url"));
}

#[test]
fn interface_enumeration_failure_is_not_reported_as_no_devices() {
    struct BrokenInterfaces;
    impl DiscoveryBackend for BrokenInterfaces {
        fn interfaces(&self) -> Result<Vec<LocalInterface>> {
            anyhow::bail!("enumeration denied")
        }
        fn probe(&self, _: &Candidate, _: Duration) -> Result<String, JsonError> {
            panic!("no candidates")
        }
    }
    let cli = Cli::parse_from(["cmd", "status"]);
    let error = resolve_target(&cli, None, &BrokenInterfaces).unwrap_err();
    assert_eq!(error.error.code, "discovery_failed");
    assert!(error.error.message.contains("enumeration denied"));
}

fn loopback_candidate(base_url: String) -> Candidate {
    // Exercise only the native HTTP prober, not OS interface enumeration.
    Candidate {
        base_url,
        interface: "loopback-fixture".into(),
        local_ip: Ipv4Addr::LOCALHOST,
        prefixlen: 30,
        netmask: "255.255.255.252".parse().unwrap(),
    }
}

#[test]
fn native_discovery_probe_has_a_total_deadline_for_slow_chunked_bodies() {
    use std::io::Write;
    use std::sync::mpsc;
    let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let candidate = loopback_candidate(format!("http://{}", listener.local_addr().unwrap()));
    let (stop, stopped) = mpsc::channel();
    let server = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        stream
            .set_write_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        crate::client_tests::read_full_http_request(&mut stream);
        stream
            .write_all(
                b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n",
            )
            .unwrap();
        let mut chunks = 0;
        // Every chunk arrives far inside the per-read timeout, but the entire
        // valid response takes longer than the request deadline. A per-read
        // timeout alone would incorrectly finish successfully.
        for _ in 0..30 {
            if stream.write_all(b"1\r\n \r\n").is_err() {
                return chunks;
            }
            chunks += 1;
            if stopped.recv_timeout(Duration::from_millis(20)).is_ok() {
                return chunks;
            }
        }
        let body = status();
        let _ = write!(stream, "{:x}\r\n{body}\r\n0\r\n\r\n", body.len());
        chunks
    });
    let result = SystemDiscovery.probe(&candidate, Duration::from_millis(300));
    let _ = stop.send(());
    let chunks = server.join().unwrap();
    assert!(chunks >= 2, "fixture must have streamed multiple chunks");
    assert_eq!(result.unwrap_err().code, "transport_error");
}

#[test]
fn native_discovery_probe_keeps_the_one_mib_body_limit() {
    let body = " ".repeat(MAX_STATUS_BYTES as usize + 1);
    let (url, server) = crate::client_tests::response_server(format!(
        "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len(),
    ));
    let error = SystemDiscovery
        .probe(&loopback_candidate(url), Duration::from_secs(2))
        .unwrap_err();
    server.join().unwrap();
    assert_eq!(error.code, "validation_failed");
    assert!(error.message.contains("size limit"));
}
