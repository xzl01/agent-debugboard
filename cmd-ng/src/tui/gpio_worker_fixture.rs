use super::model::TuiModel;
use super::runtime::on_time_tick;
use std::time::{Duration, Instant};

pub(super) const GPIO_OK: &str =
    r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"gpio"}"#;
pub(super) const STATUS_AFTER_PUT: &str =
    r#"{"gpios":[{"name":"GP13","pin":13,"value":0,"direction":"output","note":"J16_PIN1"}]}"#;
pub(super) const ADC_EMPTY: &str =
    r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","readings":[]}"#;

pub(super) fn gpio_loop_model(url: String) -> TuiModel {
    let mut model = TuiModel::new(url, Duration::from_secs(1));
    model.width = 80;
    model.gpio_names = vec!["GP13".to_string()];
    model.gpio_levels.insert("GP13".to_string(), true);
    model.gpio_is_input.insert("GP13".to_string(), true);
    // Keep the periodic poll out of the request choreography.
    model.last_http_poll = Some(Instant::now());
    model
}

pub(super) fn run_until_idle(model: &mut TuiModel) {
    let deadline = Instant::now() + Duration::from_secs(2);
    while model.gpio_pending.is_some() {
        on_time_tick(model, Instant::now()).unwrap();
        assert!(Instant::now() < deadline, "gpio job never completed");
        std::thread::sleep(Duration::from_millis(2));
    }
}
