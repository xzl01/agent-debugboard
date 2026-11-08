use super::{
    complete_host_activity_probe, complete_probe, probe_firmware, probe_host_activity,
    HostIndicatorSnapshot, IndicatorSnapshot,
};
use std::{
    io::{Read, Write},
    net::TcpListener,
    thread,
    time::Duration,
};

fn response(body: &str) -> String {
    format!(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    )
}

fn serve(
    responses: Vec<String>,
    delay: Duration,
) -> (std::net::SocketAddr, thread::JoinHandle<()>) {
    let listener = TcpListener::bind(("127.0.0.1", 0)).expect("bind test server");
    let address = listener.local_addr().expect("read test address");
    let handle = thread::spawn(move || {
        for response in responses {
            let (mut stream, _) = listener.accept().expect("accept test request");
            let mut request = [0_u8; 1024];
            let _ = stream.read(&mut request);
            thread::sleep(delay);
            let _ = stream.write_all(response.as_bytes());
        }
    });
    (address, handle)
}

fn host_status(logic_analyzer_active: bool) -> String {
    format!(r#"{{"ok":true,"activity":{{"logic_analyzer_active":{logic_analyzer_active}}}}}"#)
}

fn firmware_status(active: Option<&str>) -> String {
    let outputs = ["5v_out", "12v_out", "vdd_5v", "20v_out"]
        .iter()
        .map(|name| {
            let active = active == Some(*name);
            format!(
                r#"{{"name":"{name}","state":"{}","value":{}}}"#,
                if active { "on" } else { "off" },
                if active { 1 } else { 0 }
            )
        })
        .collect::<Vec<_>>()
        .join(",");
    format!(r#"{{"ok":true,"power_outputs":[{outputs}]}}"#)
}

fn firmware_url(address: std::net::SocketAddr) -> url::Url {
    url::Url::parse(&format!("http://{address}/")).expect("firmware URL")
}

fn snapshot_from(status: &str) -> IndicatorSnapshot {
    let (address, server) = serve(vec![response(status)], Duration::ZERO);
    let snapshot = probe_firmware(&firmware_url(address), Duration::from_secs(1))
        .expect("parse indicator snapshot");
    server.join().expect("join test server");
    snapshot
}

#[test]
fn maps_unordered_four_rails_with_vdd_5v_as_the_only_active_rail() {
    let snapshot = snapshot_from(
        r#"{"ok":true,"power_outputs":[{"name":"20v_out","state":"off","value":0},{"name":"vdd_5v","state":"on","value":1},{"name":"5v_out","state":"off","value":0},{"name":"12v_out","state":"off","value":0}]}"#,
    );

    assert_eq!(
        snapshot.indicator(),
        super::IndicatorState::new([false, false, true, false], false)
    );
}

#[test]
fn each_main_rail_turns_on_and_off_independently() {
    for (active, expected) in [
        ("5v_out", [true, false, false, false]),
        ("12v_out", [false, true, false, false]),
        ("vdd_5v", [false, false, true, false]),
        ("20v_out", [false, false, false, true]),
    ] {
        let (address, server) = serve(
            vec![
                response(&firmware_status(Some(active))),
                response(&firmware_status(None)),
            ],
            Duration::ZERO,
        );

        let enabled = probe_firmware(&firmware_url(address), Duration::from_secs(1))
            .expect("parse enabled rail snapshot");
        assert_eq!(
            enabled.indicator(),
            super::IndicatorState::new(expected, false),
            "{active} did not turn on independently"
        );
        let disabled = probe_firmware(&firmware_url(address), Duration::from_secs(1))
            .expect("parse disabled rail snapshot");
        assert_eq!(
            disabled.indicator(),
            super::IndicatorState::new([false; 4], false),
            "{active} did not turn off independently"
        );
        server.join().expect("join rail toggle server");
    }
}

#[test]
fn explicit_state_is_authoritative_over_contradictory_values() {
    let snapshot = snapshot_from(
        r#"{"ok":true,"power_outputs":[{"name":"5v_out","state":"off","value":1},{"name":"12v_out","state":"on","value":null},{"name":"vdd_5v","state":"on","value":0},{"name":"20v_out","state":"off","value":1}]}"#,
    );

    assert_eq!(
        snapshot.indicator(),
        super::IndicatorState::new([false, true, true, false], false)
    );
}

#[test]
fn unrelated_outputs_and_aliases_do_not_change_main_rails() {
    let snapshot = snapshot_from(
        r#"{"ok":true,"power_outputs":[{"name":"5v_out","state":"off","value":0},{"name":"12v_out","state":"off","value":0},{"name":"vdd_5v","state":"off","value":0},{"name":"20v_out","state":"off","value":0},{"name":"5v_otg","state":"on","value":1},{"name":"vdd5v","state":"on","value":1},{"name":"usb_hub","state":"on","value":1}],"board_monitoring":{"runtime":2}}"#,
    );

    assert_eq!(
        snapshot.indicator(),
        super::IndicatorState::new([false; 4], false)
    );
}

#[test]
fn board_status_probe_reads_the_firmware_origin_directly() {
    let listener = TcpListener::bind(("127.0.0.1", 0)).expect("bind firmware server");
    let address = listener.local_addr().expect("read firmware address");
    let board_url = firmware_url(address);
    let body = response(&firmware_status(Some("5v_out")));
    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().expect("accept firmware request");
        let mut request = [0_u8; 1024];
        let read = stream.read(&mut request).expect("read firmware request");
        let request = std::str::from_utf8(&request[..read]).expect("firmware request is UTF-8");
        assert!(request.starts_with("GET /api/v1/status "));
        stream
            .write_all(body.as_bytes())
            .expect("write firmware response");
    });

    let snapshot = super::probe_firmware(&board_url, Duration::from_secs(1))
        .expect("parse direct firmware snapshot");

    assert_eq!(
        snapshot.indicator(),
        super::IndicatorState::new([true, false, false, false], false)
    );
    server.join().expect("join firmware server");
}

#[test]
fn fast_host_activity_probe_never_requests_board_status() {
    let listener = TcpListener::bind(("127.0.0.1", 0)).expect("bind test server");
    let address = listener.local_addr().expect("read test address");
    let body = response(&host_status(true));
    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().expect("accept Host request");
        let mut request = [0_u8; 1024];
        let read = stream.read(&mut request).expect("read Host request");
        let request = std::str::from_utf8(&request[..read]).expect("Host request is UTF-8");
        assert!(request.starts_with("GET /host/api/v1/status "));
        stream
            .write_all(body.as_bytes())
            .expect("write Host response");
    });

    assert_eq!(
        probe_host_activity(address, Duration::from_secs(1)),
        Some(HostIndicatorSnapshot::new(true)),
    );
    server.join().expect("join Host server");
}

#[test]
fn host_activity_depends_only_on_the_activity_field() {
    for (active_sessions, logic_analyzer_active) in [(0, true), (99, true), (99, false)] {
        let status = format!(
            r#"{{"ok":true,"serial_logging":{{"active_sessions":{active_sessions}}},"activity":{{"logic_analyzer_active":{logic_analyzer_active}}}}}"#
        );
        let (address, server) = serve(vec![response(&status)], Duration::ZERO);

        assert_eq!(
            probe_host_activity(address, Duration::from_secs(1)),
            Some(HostIndicatorSnapshot::new(logic_analyzer_active)),
            "serial logging changed the Host activity result"
        );
        server.join().expect("join Host activity server");
    }
}

#[test]
fn complete_host_activity_preserves_all_four_rails() {
    let rails = [true, false, true, false];

    assert_eq!(
        complete_host_activity_probe(
            super::IndicatorState::new(rails, false),
            Some(HostIndicatorSnapshot::new(true))
        ),
        super::IndicatorState::new(rails, true)
    );
    assert_eq!(
        complete_host_activity_probe(super::IndicatorState::new(rails, false), None),
        super::IndicatorState::new(rails, false)
    );
    assert_eq!(complete_probe(None), super::IndicatorState::default());
}

#[test]
fn invalid_or_incomplete_main_rail_states_fail_closed() {
    let mut invalid_responses = vec![
        r#"{"ok":true,"power_outputs":[{"name":"5v_out","state":"off","value":0},{"name":"12v_out","state":"off","value":0},{"name":"vdd_5v","state":"off","value":0}]}"#.to_string(),
        r#"{"ok":true,"power_outputs":[{"name":"5v_out","state":"off","value":0},{"name":"12v_out","state":"off","value":0},{"name":"vdd_5v","value":1},{"name":"20v_out","state":"off","value":0}]}"#.to_string(),
    ];
    for state in ["unknown", "locked", "blocked"] {
        invalid_responses.push(format!(
            r#"{{"ok":true,"power_outputs":[{{"name":"5v_out","state":"off","value":0}},{{"name":"12v_out","state":"off","value":0}},{{"name":"vdd_5v","state":"{state}","value":1}},{{"name":"20v_out","state":"off","value":0}}]}}"#
        ));
    }

    for body in invalid_responses {
        let (address, server) = serve(vec![response(&body)], Duration::ZERO);
        assert!(probe_firmware(&firmware_url(address), Duration::from_millis(100)).is_none());
        server.join().expect("join invalid rail server");
    }
}

#[test]
fn probe_failures_return_no_sample() {
    let refused = TcpListener::bind(("127.0.0.1", 0)).expect("bind refused port");
    let refused_address = refused.local_addr().expect("read refused address");
    drop(refused);
    assert!(probe_firmware(&firmware_url(refused_address), Duration::from_millis(50)).is_none());
    assert!(probe_host_activity(refused_address, Duration::from_millis(50)).is_none());

    for body in ["{", r#"{"ok":true}"#, r#"{"ok":false,"power_outputs":[]}"#] {
        let (address, server) = serve(vec![response(body)], Duration::ZERO);
        assert!(probe_firmware(&firmware_url(address), Duration::from_millis(100)).is_none());
        server.join().expect("join invalid server");
    }

    for body in [
        r#"{"ok":true}"#,
        r#"{"ok":false,"activity":{"logic_analyzer_active":true}}"#,
        r#"{"ok":true,"activity":{}}"#,
    ] {
        let (address, server) = serve(vec![response(body)], Duration::ZERO);
        assert!(probe_host_activity(address, Duration::from_millis(100)).is_none());
        server.join().expect("join invalid Host status server");
    }

    let (address, server) = serve(
        vec![
            "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                .to_string(),
        ],
        Duration::ZERO,
    );
    assert!(probe_firmware(&firmware_url(address), Duration::from_millis(100)).is_none());
    server.join().expect("join non-2xx server");

    let (address, server) = serve(
        vec![response(r#"{"ok":true,"power_outputs":[]}"#)],
        Duration::from_millis(100),
    );
    assert!(probe_firmware(&firmware_url(address), Duration::from_millis(20)).is_none());
    server.join().expect("join timeout server");
}
