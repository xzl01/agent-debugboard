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

fn host_status(active_sessions: usize, logic_analyzer_sessions: usize) -> String {
    format!(
        r#"{{"ok":true,"serial_logging":{{"active_sessions":{active_sessions}}},"activity":{{"logic_analyzer_sessions":{logic_analyzer_sessions},"logic_analyzer_active":{}}}}}"#,
        logic_analyzer_sessions > 0
    )
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
fn maps_three_main_rails_from_firmware_status() {
    let snapshot = snapshot_from(
        r#"{"ok":true,"power_outputs":[{"name":"20v_out","state":"on","value":1},{"name":"vdd_5v","state":"on","value":1},{"name":"5v_out","state":"off","value":0},{"name":"12v_out","state":"on","value":1}]}"#,
    );

    assert_eq!(
        snapshot.indicator(),
        super::IndicatorState::new([false, true, true], false, false)
    );
}

#[test]
fn board_status_probe_reads_the_firmware_origin_directly() {
    let listener = TcpListener::bind(("127.0.0.1", 0)).expect("bind firmware server");
    let address = listener.local_addr().expect("read firmware address");
    let board_url = firmware_url(address);
    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().expect("accept firmware request");
        let mut request = [0_u8; 1024];
        let read = stream.read(&mut request).expect("read firmware request");
        let request = std::str::from_utf8(&request[..read]).expect("firmware request is UTF-8");
        assert!(request.starts_with("GET /api/v1/status "));
        stream
            .write_all(
                response(
                    r#"{"ok":true,"power_outputs":[{"name":"5v_out","state":"on","value":1}]}"#,
                )
                .as_bytes(),
            )
            .expect("write firmware response");
    });

    let snapshot = super::probe_firmware(&board_url, Duration::from_secs(1))
        .expect("parse direct firmware snapshot");

    assert_eq!(
        snapshot.indicator(),
        super::IndicatorState::new([true, false, false], false, false)
    );
    server.join().expect("join firmware server");
}

#[test]
fn fast_host_activity_probe_never_requests_board_status() {
    let listener = TcpListener::bind(("127.0.0.1", 0)).expect("bind test server");
    let address = listener.local_addr().expect("read test address");
    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().expect("accept Host request");
        let mut request = [0_u8; 1024];
        let read = stream.read(&mut request).expect("read Host request");
        let request = std::str::from_utf8(&request[..read]).expect("Host request is UTF-8");
        assert!(request.starts_with("GET /host/api/v1/status "));
        stream
            .write_all(response(&host_status(1, 1)).as_bytes())
            .expect("write Host response");
    });

    assert_eq!(
        probe_host_activity(address, Duration::from_secs(1)),
        Some(HostIndicatorSnapshot::new(true, true)),
    );
    server.join().expect("join Host server");
}

#[test]
fn bridge_and_logic_state_map_directly_without_a_delta_baseline() {
    let inactive = super::IndicatorState::new([false, false, false], false, false);

    assert_eq!(
        complete_host_activity_probe(inactive, Some(HostIndicatorSnapshot::new(true, true))),
        super::IndicatorState::new([false, false, false], true, true)
    );
    assert_eq!(complete_probe(None), super::IndicatorState::default());
}

#[test]
fn power_output_order_and_unrelated_fields_do_not_change_mapping() {
    let snapshot = snapshot_from(
        r#"{"ok":true,"power_outputs":[{"name":"12v_out","state":"off","value":0,"signal":"metadata"},{"name":"20v_out","state":"off","value":1},{"name":"5v_out","state":"on","value":null}],"board_monitoring":{"runtime":2}}"#,
    );

    assert_eq!(
        snapshot.indicator(),
        super::IndicatorState::new([true, false, true], false, false)
    );
}

#[test]
fn probe_failures_return_no_sample() {
    let refused = TcpListener::bind(("127.0.0.1", 0)).expect("bind refused port");
    let refused_address = refused.local_addr().expect("read refused address");
    drop(refused);
    assert!(probe_firmware(&firmware_url(refused_address), Duration::from_millis(50)).is_none());

    for body in ["{", r#"{"ok":true}"#, r#"{"ok":false,"power_outputs":[]}"#] {
        let (address, server) = serve(vec![response(body)], Duration::ZERO);
        assert!(probe_firmware(&firmware_url(address), Duration::from_millis(100)).is_none());
        server.join().expect("join invalid server");
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

    let (address, server) = serve(vec![response(r#"{"ok":true}"#)], Duration::ZERO);
    assert!(probe_host_activity(address, Duration::from_millis(100)).is_none());
    server.join().expect("join invalid Host status server");

    let (address, server) = serve(
        vec![response(r#"{"ok":true,"power_outputs":[]}"#)],
        Duration::from_millis(100),
    );
    assert!(probe_firmware(&firmware_url(address), Duration::from_millis(20)).is_none());
    server.join().expect("join timeout server");
}
