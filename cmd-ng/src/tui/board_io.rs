use super::TuiActionMsg;
use crate::client::{BoardRequest, BoardTransport};
use anyhow::Result;
use reqwest::{Method, Url};
use std::time::Duration;

fn board_path(kind: &str, name: &str) -> Result<String> {
    if matches!(name, "." | "..") {
        anyhow::bail!("invalid {kind} name {name:?}: dot path segments are not allowed");
    }

    let mut url = Url::parse("http://localhost/api/v1")?;
    {
        let mut segments = url
            .path_segments_mut()
            .map_err(|()| anyhow::anyhow!("build board I/O request path"))?;
        segments.push(kind).push(name);
    }
    Ok(url.path().to_string())
}

pub(super) fn perform_control_action(
    base_url: &str,
    timeout: Duration,
    output: &str,
    current_state: bool,
) -> Result<TuiActionMsg> {
    let next_state = if current_state { "off" } else { "on" };
    let path = board_path("power", output)?;
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path,
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
    let path = board_path("switch", name)?;
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path,
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
    let path = board_path("gpio", gpio)?;
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path,
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
    let path = board_path("gpio", gpio)?;
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path,
        query: vec![],
        body: Some(serde_json::json!({ "direction": "input" })),
    })?;
    Ok(TuiActionMsg {
        status: format!("gpio {}=input", gpio),
        err: None,
    })
}
