use super::config_result::ConfigJobKind;
use super::config_state::ConfigRequest;
use super::gpio_fixture::{control_rect, draw, projection_model};
use super::mock_board::{mock_server, Reply};
use super::mouse_events::handle_mouse_at;
use super::runtime::{on_time_tick, start_config_request};
use crate::client::{BoardClient, BoardRequest, BoardTransport};
use anyhow::Result;
use crossterm::event::{KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use std::time::{Duration, Instant};

fn mouse(kind: MouseEventKind, column: u16, row: u16) -> MouseEvent {
    MouseEvent {
        kind,
        column,
        row,
        modifiers: KeyModifiers::NONE,
    }
}

fn begin_gpio_gesture(
    model: &mut super::model::TuiModel,
    rect: ratatui::layout::Rect,
    start: Instant,
) -> Result<()> {
    handle_mouse_at(
        model,
        mouse(MouseEventKind::Down(MouseButton::Left), rect.x, rect.y),
        start,
    )?;
    Ok(())
}

#[test]
fn starting_any_saved_config_request_cancels_active_gpio_gesture() -> Result<()> {
    let requests = [
        ConfigRequest::Save {
            items: vec![],
            confirm: false,
        },
        ConfigRequest::Clear,
        ConfigRequest::Refresh,
    ];
    for request in requests {
        for await_second in [false, true] {
            let start = Instant::now();
            let mut model = projection_model()?;
            model.base_url = "http://127.0.0.1:9".to_string();
            model.timeout = Duration::from_millis(30);
            draw(&mut model, 80, 24)?;
            let rect = control_rect(&model, "GP10")?;
            begin_gpio_gesture(&mut model, rect, start)?;
            if await_second {
                handle_mouse_at(
                    &mut model,
                    mouse(MouseEventKind::Up(MouseButton::Left), rect.x, rect.y),
                    start,
                )?;
            }
            assert!(model.gpio_gesture.is_active());

            start_config_request(&mut model, request.clone());

            assert!(!model.gpio_gesture.is_active());
            assert!(model.gpio_pending.is_none());
            assert!(model.saved_config.busy.is_some());
        }
    }
    Ok(())
}

#[test]
fn saved_config_busy_prevents_overdue_gpio_put() -> Result<()> {
    const GPIO_OK: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"gpio"}"#;
    let (url, requests) = mock_server(vec![Reply::Http(200, GPIO_OK)]);
    let start = Instant::now();
    let mut model = projection_model()?;
    model.base_url = url.clone();
    model.timeout = Duration::from_secs(1);
    model.last_http_poll = Some(start);
    draw(&mut model, 80, 24)?;
    let rect = control_rect(&model, "GP10")?;
    begin_gpio_gesture(&mut model, rect, start)?;
    handle_mouse_at(
        &mut model,
        mouse(MouseEventKind::Up(MouseButton::Left), rect.x, rect.y),
        start,
    )?;
    model.saved_config.busy = Some(ConfigJobKind::Clear);

    on_time_tick(&mut model, start + Duration::from_millis(220))?;

    assert!(
        model.gpio_pending.is_none(),
        "busy tick must not start a GPIO worker"
    );
    let client = BoardClient::new(&url, Duration::from_secs(1))?;
    client.send_text(BoardRequest {
        method: reqwest::Method::GET,
        path: "/test-shutdown".to_string(),
        query: vec![],
        body: None,
    })?;
    let request = requests.recv_timeout(Duration::from_secs(1))?;
    assert!(
        request.starts_with("GET /test-shutdown"),
        "busy tick emitted an unexpected request: {request}"
    );
    Ok(())
}
