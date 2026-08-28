use super::actions::expire_hardware_confirmation;
use super::config_state;
use super::events::{activate_gpio_gesture, handle_key, KeyOutcome};
use super::gpio_io;
use super::model::TuiModel;
use super::mouse_events::{handle_mouse_at, MouseOutcome};
use super::render;
use super::TUI_POLL_INTERVAL;
use crate::client::{BoardRequest, BoardTransport};
use crate::ws_status::WsStatusSnapshot;
use anyhow::Result;
use crossterm::event::{self, Event};
use ratatui::{backend::CrosstermBackend, Terminal};
use std::io::Stdout;
use std::time::{Duration, Instant};

const HTTP_POLL_INTERVAL: Duration = Duration::from_secs(2);
pub(super) const READY_EVENT_DRAIN_LIMIT: usize = 64;

#[derive(Debug, PartialEq, Eq)]
pub(super) enum EventDisposition {
    Continue,
    Redraw,
    Exit,
}

pub(super) fn event_loop(
    model: &mut TuiModel,
    terminal: &mut Terminal<CrosstermBackend<Stdout>>,
) -> Result<u8> {
    let mut last_tick = Instant::now() - TUI_POLL_INTERVAL;

    loop {
        match drain_ready_events(model, || {
            if event::poll(Duration::ZERO)? {
                Ok(Some((event::read()?, Instant::now())))
            } else {
                Ok(None)
            }
        })? {
            EventDisposition::Continue => {}
            EventDisposition::Redraw => {
                draw_ui(terminal, model)?;
                continue;
            }
            EventDisposition::Exit => return Ok(0),
        }
        if last_tick.elapsed() >= TUI_POLL_INTERVAL {
            on_time_tick(model, Instant::now())?;
            last_tick = Instant::now();
        }

        draw_ui(terminal, model)?;

        if event::poll(Duration::from_millis(50))? {
            match process_event(model, event::read()?, Instant::now())? {
                EventDisposition::Continue => {}
                EventDisposition::Redraw => draw_ui(terminal, model)?,
                EventDisposition::Exit => return Ok(0),
            }
        }
    }
}

fn draw_ui(terminal: &mut Terminal<CrosstermBackend<Stdout>>, model: &mut TuiModel) -> Result<()> {
    terminal.draw(|frame| {
        model.width = frame.area().width as usize;
        model.height = frame.area().height as usize;
        render::render_ui(frame, model);
    })?;
    Ok(())
}

pub(super) fn drain_ready_events(
    model: &mut TuiModel,
    mut next: impl FnMut() -> Result<Option<(Event, Instant)>>,
) -> Result<EventDisposition> {
    for _ in 0..READY_EVENT_DRAIN_LIMIT {
        let Some((event, now)) = next()? else {
            break;
        };
        match process_event(model, event, now)? {
            EventDisposition::Continue => {}
            EventDisposition::Redraw => return Ok(EventDisposition::Redraw),
            EventDisposition::Exit => return Ok(EventDisposition::Exit),
        }
    }
    Ok(EventDisposition::Continue)
}

pub(super) fn on_time_tick(model: &mut TuiModel, now: Instant) -> Result<()> {
    if let Some(result) = model.config_worker.poll() {
        let outcome = model.saved_config.finish(result);
        model.status = outcome.status().to_string();
        match outcome {
            super::config_result::ConfigOutcome::AwaitingConfirmation
            | super::config_result::ConfigOutcome::Failed => {
                model.hardware_confirm = None;
                model.gpio_gesture.cancel();
            }
            super::config_result::ConfigOutcome::Refreshed
            | super::config_result::ConfigOutcome::Saved
            | super::config_result::ConfigOutcome::Cleared => {}
        }
    }

    if let Some(result) = model.gpio_worker.poll() {
        finish_gpio_job(model, result);
    }

    if gesture_can_fire(model) {
        if let Some(action) = model.gpio_gesture.tick(now) {
            if model.gpio_names.iter().any(|pin| pin == &action.pin) {
                activate_gpio_gesture(model, action)?;
            }
        }
    } else {
        model.gpio_gesture.cancel();
    }

    expire_hardware_confirmation(model, now);

    if model.closed || model.paused {
        return Ok(());
    }

    let should_poll = match model.last_http_poll {
        Some(t) => t.elapsed() >= HTTP_POLL_INTERVAL,
        None => true,
    };
    let gesture_defers_poll = model.gpio_gesture.is_active()
        && model.gpio_poll_defer_until.is_some_and(|until| now < until);
    if should_poll && !gesture_defers_poll {
        poll_http(model)?;
    }
    Ok(())
}

pub(super) fn process_event(
    model: &mut TuiModel,
    event: Event,
    now: Instant,
) -> Result<EventDisposition> {
    match event {
        Event::Key(key) => match handle_key(model, key, now)? {
            KeyOutcome::Continue => Ok(EventDisposition::Continue),
            KeyOutcome::Redraw => Ok(EventDisposition::Redraw),
            KeyOutcome::Exit => Ok(EventDisposition::Exit),
        },
        Event::Mouse(mouse) => match handle_mouse_at(model, mouse, now)? {
            MouseOutcome::Continue => Ok(EventDisposition::Continue),
            MouseOutcome::Redraw => Ok(EventDisposition::Redraw),
        },
        Event::Resize(_, _) => {
            model.gpio_gesture.cancel();
            model.hit_map.clear();
            Ok(EventDisposition::Redraw)
        }
        _ => Ok(EventDisposition::Continue),
    }
}

fn gesture_can_fire(model: &TuiModel) -> bool {
    model.gpio_pending.is_none()
        && model.hardware_confirm.is_none()
        && model.saved_config.busy.is_none()
        && model.saved_config.confirmation().is_none()
        && model.saved_config.error.is_none()
        && !model.paused
        && model.active_page == super::pages::ActivePage::Controls
}

pub(super) fn poll_http(model: &mut TuiModel) -> Result<()> {
    model.last_http_poll = Some(Instant::now());
    let client = crate::client::BoardClient::new(&model.base_url, model.timeout)?;
    let status_data = client.send_text(BoardRequest {
        method: reqwest::Method::GET,
        path: "/api/v1/status".to_string(),
        query: vec![],
        body: None,
    })?;
    let status_snapshot: WsStatusSnapshot = serde_json::from_str(&status_data)?;
    let config_changed = model.apply_status_snapshot(status_snapshot);

    let adc_data = client.send_text(BoardRequest {
        method: reqwest::Method::GET,
        path: "/api/v1/adc/read".to_string(),
        query: vec![],
        body: None,
    })?;
    let adc_response = crate::adc::transform_response(&adc_data).map_err(|e| anyhow::anyhow!(e))?;
    model.apply_adc_response(adc_response.readings);

    if model.hardware_confirm.is_none() {
        model.status = "HTTP mode".to_string();
    }
    model.err = None;
    if config_changed {
        if let Some(request) = model.saved_config.request_refresh() {
            start_config_request(model, request);
        }
    }
    Ok(())
}

fn finish_gpio_job(model: &mut TuiModel, result: gpio_io::GpioJobResult) {
    let refresh = poll_http(model);
    match result.outcome {
        Ok(msg) => {
            model.apply_action_msg(msg);
            if let Err(error) = refresh {
                model.err = Some(error.to_string());
            }
        }
        Err(message) => {
            model.err = Some(message.clone());
            model.status = message;
        }
    }
    model.gpio_pending = None;
}

pub(super) fn start_config_request(model: &mut TuiModel, request: config_state::ConfigRequest) {
    let kind = request.kind();
    model.gpio_gesture.cancel();
    if model
        .config_worker
        .start(model.base_url.clone(), model.timeout, request)
    {
        model.saved_config.start(kind);
        model.status = format!("Saved Config {}…", kind.as_str());
    }
}
