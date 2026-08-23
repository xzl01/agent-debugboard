use super::TuiActionMsg;
use crate::client::BoardRequest;
use anyhow::Result;
use reqwest::Method;
use std::time::Duration;

pub(super) fn perform_control_action(
    base_url: &str,
    timeout: Duration,
    output: &str,
    current_state: bool,
) -> Result<TuiActionMsg> {
    let next_state = if current_state { "off" } else { "on" };
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path: format!("/api/v1/power/{output}"),
        query: vec![],
        body: Some(serde_json::json!({ "state": next_state })),
    })?;
    Ok(TuiActionMsg {
        status: format!("power {output}={next_state}"),
        err: None,
    })
}

pub(super) fn set_switch_route(
    base_url: &str,
    timeout: Duration,
    name: &str,
    route: &str,
) -> Result<TuiActionMsg> {
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path: format!("/api/v1/switch/{name}"),
        query: vec![],
        body: Some(serde_json::json!({ "route": route })),
    })?;
    Ok(TuiActionMsg {
        status: format!("switch {name}={route}"),
        err: None,
    })
}

pub(super) fn set_gpio_output(
    base_url: &str,
    timeout: Duration,
    gpio: &str,
    value: bool,
) -> Result<TuiActionMsg> {
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path: format!("/api/v1/gpio/{gpio}"),
        query: vec![],
        body: Some(
            serde_json::json!({ "direction": "output", "value": if value { 1 } else { 0 } }),
        ),
    })?;
    Ok(TuiActionMsg {
        status: format!("gpio {}={}", gpio, if value { 1 } else { 0 }),
        err: None,
    })
}

pub(super) fn set_gpio_input(
    base_url: &str,
    timeout: Duration,
    gpio: &str,
) -> Result<TuiActionMsg> {
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path: format!("/api/v1/gpio/{gpio}"),
        query: vec![],
        body: Some(serde_json::json!({ "direction": "input" })),
    })?;
    Ok(TuiActionMsg {
        status: format!("gpio {}=input", gpio),
        err: None,
    })
}
