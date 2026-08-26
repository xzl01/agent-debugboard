use super::gpio_io::{GpioAction, GpioJob, GpioWorker};
use super::mock_board::{mock_server, Reply};
use std::sync::mpsc::Receiver;
use std::time::{Duration, Instant};

const GPIO_OK: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"gpio"}"#;

fn job(action: GpioAction) -> GpioJob {
    GpioJob {
        action,
        target: "GP13".to_string(),
    }
}

fn poll_until(worker: &mut GpioWorker) -> super::gpio_io::GpioJobResult {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        if let Some(result) = worker.poll() {
            return result;
        }
        assert!(Instant::now() < deadline, "worker never produced a result");
        std::thread::sleep(Duration::from_millis(2));
    }
}

fn put_request(requests: &Receiver<String>) -> String {
    let request = requests.recv().unwrap();
    assert!(request.starts_with("PUT /api/v1/gpio/GP13 HTTP/1.1"));
    request
}

#[test]
fn drive_low_puts_output_value_zero() {
    let (url, requests) = mock_server(vec![Reply::Http(200, GPIO_OK)]);
    let mut worker = GpioWorker::new();
    assert!(worker.start(url, Duration::from_secs(1), job(GpioAction::DriveLow)));

    let result = poll_until(&mut worker);

    let outcome = result.outcome.unwrap();
    assert_eq!(outcome.status, "gpio GP13=0");
    assert!(outcome.err.is_none());
    let request = put_request(&requests);
    assert!(request.contains(r#"{"direction":"output","value":0}"#));
}

#[test]
fn drive_high_puts_output_value_one() {
    let (url, requests) = mock_server(vec![Reply::Http(200, GPIO_OK)]);
    let mut worker = GpioWorker::new();
    assert!(worker.start(url, Duration::from_secs(1), job(GpioAction::DriveHigh)));

    let result = poll_until(&mut worker);

    assert_eq!(result.outcome.unwrap().status, "gpio GP13=1");
    let request = put_request(&requests);
    assert!(request.contains(r#"{"direction":"output","value":1}"#));
}

#[test]
fn set_input_puts_direction_input() {
    let (url, requests) = mock_server(vec![Reply::Http(200, GPIO_OK)]);
    let mut worker = GpioWorker::new();
    assert!(worker.start(url, Duration::from_secs(1), job(GpioAction::SetInput)));

    let result = poll_until(&mut worker);

    assert_eq!(result.outcome.unwrap().status, "gpio GP13=input");
    let request = put_request(&requests);
    assert!(request.contains(r#"{"direction":"input"}"#));
    assert!(!request.contains("value"));
}

#[test]
fn second_start_is_rejected_while_a_job_is_in_flight() {
    let (url, requests) = mock_server(vec![Reply::Slow(200, GPIO_OK)]);
    let mut worker = GpioWorker::new();
    assert!(worker.start(
        url.clone(),
        Duration::from_secs(1),
        job(GpioAction::DriveHigh)
    ));

    assert!(
        !worker.start(url, Duration::from_secs(1), job(GpioAction::DriveLow)),
        "one in-flight job must reject a second start"
    );

    let result = poll_until(&mut worker);
    assert_eq!(result.outcome.unwrap().status, "gpio GP13=1");
    assert!(put_request(&requests).contains(r#""value":1"#));
    assert!(requests.recv_timeout(Duration::from_millis(50)).is_err());
}

#[test]
fn worker_accepts_a_new_job_after_the_previous_one_completes() {
    let (url, requests) = mock_server(vec![Reply::Http(200, GPIO_OK), Reply::Http(200, GPIO_OK)]);
    let mut worker = GpioWorker::new();
    assert!(worker.start(
        url.clone(),
        Duration::from_secs(1),
        job(GpioAction::DriveHigh)
    ));
    let first = poll_until(&mut worker);
    assert_eq!(first.outcome.unwrap().status, "gpio GP13=1");

    assert!(
        worker.start(url, Duration::from_secs(1), job(GpioAction::SetInput)),
        "a finished job must release the worker for the next one"
    );
    let second = poll_until(&mut worker);
    assert_eq!(second.outcome.unwrap().status, "gpio GP13=input");

    assert!(put_request(&requests).contains(r#""value":1"#));
    assert!(put_request(&requests).contains(r#""direction":"input""#));
}

#[test]
fn transport_failure_is_reported_as_a_job_error() {
    let (url, requests) = mock_server(vec![Reply::Disconnect]);
    let mut worker = GpioWorker::new();
    assert!(worker.start(url, Duration::from_secs(1), job(GpioAction::DriveHigh)));

    let result = poll_until(&mut worker);

    let error = result.outcome.unwrap_err();
    assert!(
        error.starts_with("gpio GP13=1 failed:"),
        "unexpected error text: {error}"
    );
    put_request(&requests);
}

#[test]
fn poll_returns_the_result_queued_between_empty_and_finished_checks() {
    use std::sync::{Arc, Barrier};

    let gate = Arc::new(Barrier::new(2));
    let (url, requests) = mock_server(vec![
        Reply::Gated(200, GPIO_OK, gate.clone()),
        Reply::Http(200, GPIO_OK),
    ]);
    let mut worker = GpioWorker::new();
    assert!(worker.start(
        url.clone(),
        Duration::from_secs(1),
        job(GpioAction::DriveHigh)
    ));

    // Force the race deterministically: the first try_recv returns Empty, then
    // the worker sends its result and finishes before the finished check runs.
    let hook_gate = gate.clone();
    worker.empty_hook = Some(Box::new(move |worker: &GpioWorker| {
        hook_gate.wait();
        let deadline = Instant::now() + Duration::from_secs(2);
        while !worker.active_finished() {
            assert!(Instant::now() < deadline, "worker never finished");
            std::thread::sleep(Duration::from_millis(1));
        }
    }));

    let result = poll_until(&mut worker);
    assert_eq!(
        result.outcome.unwrap().status,
        "gpio GP13=1",
        "the real queued result must win over the stopped placeholder"
    );
    assert!(put_request(&requests).contains(r#""value":1"#));

    assert!(
        worker.poll().is_none(),
        "the queued result must be consumed exactly once"
    );

    assert!(
        worker.start(url, Duration::from_secs(1), job(GpioAction::SetInput)),
        "a drained worker must accept the next job"
    );
    let result = poll_until(&mut worker);
    assert_eq!(
        result.outcome.unwrap().status,
        "gpio GP13=input",
        "no stale result may be applied to the next job"
    );
    assert!(put_request(&requests).contains(r#""direction":"input""#));
}
