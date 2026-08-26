use super::actions::{activate_item, ControlIntent};
use super::controls::ControlItem;
use super::gpio_worker_fixture::{
    gpio_loop_model, run_until_idle, ADC_EMPTY, GPIO_OK, STATUS_AFTER_PUT,
};
use super::mock_board::{mock_server, Reply};
use super::runtime::on_time_tick;
use std::time::{Duration, Instant};

fn activate_gpio(model: &mut super::model::TuiModel, intent: ControlIntent) {
    activate_item(model, ControlItem::Gpio("GP13".to_string()), intent).unwrap();
}

#[test]
fn gpio_worker_result_triggers_status_get_then_clears_pending() {
    let (url, requests) = mock_server(vec![
        Reply::Http(200, GPIO_OK),
        Reply::Http(200, STATUS_AFTER_PUT),
        Reply::Http(200, ADC_EMPTY),
    ]);
    let mut model = gpio_loop_model(url);

    activate_gpio(&mut model, ControlIntent::DriveHigh);
    assert!(model.gpio_pending.is_some());
    assert_eq!(
        model.gpio_is_input.get("GP13"),
        Some(&true),
        "no optimistic direction write before the worker result"
    );

    run_until_idle(&mut model);

    assert!(model.gpio_pending.is_none());
    assert_eq!(model.status, "gpio GP13=1");
    assert_eq!(model.err, None);
    assert!(requests
        .recv()
        .unwrap()
        .starts_with("PUT /api/v1/gpio/GP13"));
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/status"));
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/adc/read"));
    assert_eq!(
        model.gpio_levels.get("GP13"),
        Some(&false),
        "the authoritative snapshot wins over the requested HIGH"
    );
    assert_eq!(model.gpio_is_input.get("GP13"), Some(&false));
}

#[test]
fn gpio_put_failure_still_refreshes_and_never_retries() {
    let (url, requests) = mock_server(vec![
        Reply::Disconnect,
        Reply::Http(200, STATUS_AFTER_PUT),
        Reply::Http(200, ADC_EMPTY),
    ]);
    let mut model = gpio_loop_model(url);

    activate_gpio(&mut model, ControlIntent::DriveHigh);
    run_until_idle(&mut model);

    assert!(model.gpio_pending.is_none());
    assert!(
        model.status.starts_with("gpio GP13=1 failed:"),
        "unexpected status: {}",
        model.status
    );
    assert!(model.err.is_some());
    assert!(requests
        .recv()
        .unwrap()
        .starts_with("PUT /api/v1/gpio/GP13"));
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/status"));
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/adc/read"));
    assert!(
        requests.recv_timeout(Duration::from_millis(50)).is_err(),
        "a failed PUT must never be retried"
    );
    assert_eq!(model.gpio_levels.get("GP13"), Some(&false));
    assert_eq!(model.gpio_is_input.get("GP13"), Some(&false));
}

#[test]
fn gpio_refresh_failure_still_clears_pending() {
    let (url, requests) = mock_server(vec![Reply::Http(200, GPIO_OK), Reply::Disconnect]);
    let mut model = gpio_loop_model(url);

    activate_gpio(&mut model, ControlIntent::DriveHigh);
    run_until_idle(&mut model);

    assert!(model.gpio_pending.is_none());
    assert_eq!(model.status, "gpio GP13=1");
    assert!(model.err.is_some(), "the refresh error must surface");
    assert!(requests
        .recv()
        .unwrap()
        .starts_with("PUT /api/v1/gpio/GP13"));
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/status"));
    assert_eq!(
        model.gpio_levels.get("GP13"),
        Some(&true),
        "a failed refresh leaves the previous maps untouched"
    );
    assert_eq!(model.gpio_is_input.get("GP13"), Some(&true));
}

#[test]
fn gpio_worker_result_refreshes_even_while_paused() {
    let (url, requests) = mock_server(vec![
        Reply::Http(200, GPIO_OK),
        Reply::Http(200, STATUS_AFTER_PUT),
        Reply::Http(200, ADC_EMPTY),
    ]);
    let mut model = gpio_loop_model(url);
    model.paused = true;

    activate_gpio(&mut model, ControlIntent::DriveHigh);
    run_until_idle(&mut model);

    assert!(model.gpio_pending.is_none());
    assert!(requests
        .recv()
        .unwrap()
        .starts_with("PUT /api/v1/gpio/GP13"));
    assert!(
        requests.recv().unwrap().starts_with("GET /api/v1/status"),
        "the post-job refresh must run even while paused"
    );
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/adc/read"));
    assert_eq!(model.gpio_levels.get("GP13"), Some(&false));

    for _ in 0..3 {
        on_time_tick(&mut model, Instant::now()).unwrap();
    }
    assert!(
        requests.recv_timeout(Duration::from_millis(50)).is_err(),
        "the periodic poll stays suppressed while paused"
    );
}

#[test]
fn gpio_duplicate_activation_sends_exactly_one_put() {
    let (url, requests) = mock_server(vec![
        Reply::Slow(200, GPIO_OK),
        Reply::Http(200, STATUS_AFTER_PUT),
        Reply::Http(200, ADC_EMPTY),
    ]);
    let mut model = gpio_loop_model(url);

    activate_gpio(&mut model, ControlIntent::DriveHigh);
    activate_gpio(&mut model, ControlIntent::DriveLow);
    assert!(
        model.status.contains("dropped"),
        "the duplicate must be visibly dropped: {}",
        model.status
    );

    run_until_idle(&mut model);

    assert!(model.gpio_pending.is_none());
    let put = requests.recv().unwrap();
    assert!(put.starts_with("PUT /api/v1/gpio/GP13"));
    assert!(put.contains(r#""value":1"#));
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/status"));
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/adc/read"));
    assert!(
        requests.recv_timeout(Duration::from_millis(50)).is_err(),
        "the dropped duplicate must never reach the board"
    );
}

#[test]
fn gpio_worker_accepts_the_next_job_after_completion() {
    let (url, requests) = mock_server(vec![
        Reply::Http(200, GPIO_OK),
        Reply::Http(200, STATUS_AFTER_PUT),
        Reply::Http(200, ADC_EMPTY),
        Reply::Http(200, GPIO_OK),
        Reply::Http(200, STATUS_AFTER_PUT),
        Reply::Http(200, ADC_EMPTY),
    ]);
    let mut model = gpio_loop_model(url);

    activate_gpio(&mut model, ControlIntent::DriveHigh);
    run_until_idle(&mut model);
    activate_gpio(&mut model, ControlIntent::DriveLow);
    assert!(
        model.gpio_pending.is_some(),
        "a completed job must release the worker for the next one"
    );
    run_until_idle(&mut model);

    assert!(model.gpio_pending.is_none());
    assert_eq!(model.status, "gpio GP13=0");
    assert!(requests.recv().unwrap().contains(r#""value":1"#));
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/status"));
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/adc/read"));
    assert!(requests.recv().unwrap().contains(r#""value":0"#));
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/status"));
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/adc/read"));
}
