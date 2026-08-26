use super::actions::{activate_item, ControlIntent};
use super::controls::ControlItem;
use super::gpio_fixture::body_line;
use super::gpio_worker_fixture::{
    gpio_loop_model, run_until_idle, ADC_EMPTY, GPIO_OK, STATUS_AFTER_PUT,
};
use super::mock_board::{mock_server, Reply};
use anyhow::Result;

#[test]
fn completed_pending_job_clears_the_tag() -> Result<()> {
    let (url, _requests) = mock_server(vec![
        Reply::Http(200, GPIO_OK),
        Reply::Http(200, STATUS_AFTER_PUT),
        Reply::Http(200, ADC_EMPTY),
    ]);
    let mut model = gpio_loop_model(url);

    activate_item(
        &mut model,
        ControlItem::Gpio("GP13".to_string()),
        ControlIntent::DriveHigh,
    )?;
    assert!(model.gpio_pending.is_some());
    let line = body_line(&model, 80, "GP13")?;
    assert!(line.contains("[HIGH…]"), "in-flight tag missing: {line:?}");

    run_until_idle(&mut model);
    assert!(model.gpio_pending.is_none());
    assert_eq!(model.status, "gpio GP13=1");
    let line = body_line(&model, 80, "GP13")?;
    assert!(
        !line.contains("[HIGH…]"),
        "a completed job must clear the tag: {line:?}"
    );
    Ok(())
}

#[test]
fn failed_pending_job_clears_the_tag_and_surfaces_the_error() -> Result<()> {
    let (url, _requests) = mock_server(vec![
        Reply::Disconnect,
        Reply::Http(200, STATUS_AFTER_PUT),
        Reply::Http(200, ADC_EMPTY),
    ]);
    let mut model = gpio_loop_model(url);

    activate_item(
        &mut model,
        ControlItem::Gpio("GP13".to_string()),
        ControlIntent::DriveHigh,
    )?;
    let line = body_line(&model, 80, "GP13")?;
    assert!(line.contains("[HIGH…]"), "in-flight tag missing: {line:?}");

    run_until_idle(&mut model);
    assert!(model.gpio_pending.is_none());
    assert!(
        model.status.starts_with("gpio GP13=1 failed:"),
        "unexpected status: {}",
        model.status
    );
    assert!(model.err.is_some());
    let line = body_line(&model, 80, "GP13")?;
    assert!(
        !line.contains("[HIGH…]"),
        "a failed job must clear the tag: {line:?}"
    );
    Ok(())
}
