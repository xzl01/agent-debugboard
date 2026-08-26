use super::controls::ControlItem;
use super::gpio_fixture::{control_rect, draw, projection_model};
use super::gpio_io::{GpioAction, GpioJob};
use super::mouse_events::handle_mouse_at;
use super::runtime::{drain_ready_events, on_time_tick, EventDisposition, READY_EVENT_DRAIN_LIMIT};
use anyhow::Result;
use crossterm::event::{Event, KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use std::time::{Duration, Instant};

fn mouse(kind: MouseEventKind, column: u16, row: u16) -> MouseEvent {
    MouseEvent {
        kind,
        column,
        row,
        modifiers: KeyModifiers::NONE,
    }
}

#[test]
fn ready_event_budget_still_advances_an_overdue_hold_tick() -> Result<()> {
    let start = Instant::now();
    let mut model = projection_model()?;
    model.base_url = "http://127.0.0.1:9".to_string();
    model.timeout = Duration::from_millis(30);
    model.last_http_poll = Some(start);
    draw(&mut model, 80, 24)?;
    let rect = control_rect(&model, "GP10")?;
    handle_mouse_at(
        &mut model,
        mouse(MouseEventKind::Down(MouseButton::Left), rect.x, rect.y),
        start,
    )?;

    let mut delivered = 0usize;
    let disposition = drain_ready_events(&mut model, || {
        delivered += 1;
        Ok(Some((
            Event::Mouse(mouse(MouseEventKind::Moved, rect.x, rect.y)),
            start + Duration::from_millis(600),
        )))
    })?;
    assert_eq!(disposition, EventDisposition::Continue);
    assert_eq!(delivered, READY_EVENT_DRAIN_LIMIT);

    on_time_tick(&mut model, start + Duration::from_millis(600))?;
    assert_eq!(
        model.gpio_pending,
        Some(GpioJob {
            action: GpioAction::DriveHigh,
            target: "GP10".to_string(),
        })
    );
    Ok(())
}

#[test]
fn late_release_defers_http_for_the_complete_second_click_window() -> Result<()> {
    let start = Instant::now();
    let mut model = projection_model()?;
    model.base_url = "http://127.0.0.1:9".to_string();
    model.timeout = Duration::from_millis(30);
    let last_http_poll = start - Duration::from_secs(3);
    model.last_http_poll = Some(last_http_poll);
    draw(&mut model, 80, 24)?;
    let rect = control_rect(&model, "GP10")?;
    handle_mouse_at(
        &mut model,
        mouse(MouseEventKind::Down(MouseButton::Left), rect.x, rect.y),
        start,
    )?;
    handle_mouse_at(
        &mut model,
        mouse(MouseEventKind::Up(MouseButton::Left), rect.x, rect.y),
        start + Duration::from_millis(599),
    )?;

    on_time_tick(&mut model, start + Duration::from_millis(600))?;
    on_time_tick(&mut model, start + Duration::from_millis(818))?;
    assert_eq!(model.last_http_poll, Some(last_http_poll));
    assert_eq!(model.gpio_pending, None);
    assert!(model.gpio_gesture.is_active());
    assert_eq!(
        model.hit_map.controls.at(rect.x, rect.y),
        Some(&ControlItem::Gpio("GP10".to_string()))
    );
    Ok(())
}

#[test]
fn expired_second_down_dispatches_low_and_consumes_the_late_press() -> Result<()> {
    for delay in [220, 221] {
        let start = Instant::now();
        let mut model = projection_model()?;
        model.base_url = "http://127.0.0.1:9".to_string();
        model.timeout = Duration::from_millis(30);
        model.last_http_poll = Some(start);
        draw(&mut model, 80, 24)?;
        let rect = control_rect(&model, "GP10")?;
        handle_mouse_at(
            &mut model,
            mouse(MouseEventKind::Down(MouseButton::Left), rect.x, rect.y),
            start,
        )?;
        handle_mouse_at(
            &mut model,
            mouse(MouseEventKind::Up(MouseButton::Left), rect.x, rect.y),
            start,
        )?;
        handle_mouse_at(
            &mut model,
            mouse(MouseEventKind::Down(MouseButton::Left), rect.x, rect.y),
            start + Duration::from_millis(delay),
        )?;
        assert_eq!(
            model.gpio_pending,
            Some(GpioJob {
                action: GpioAction::DriveLow,
                target: "GP10".to_string(),
            })
        );
        handle_mouse_at(
            &mut model,
            mouse(MouseEventKind::Up(MouseButton::Left), rect.x, rect.y),
            start + Duration::from_millis(delay + 1),
        )?;
        assert_eq!(
            model.gpio_pending,
            Some(GpioJob {
                action: GpioAction::DriveLow,
                target: "GP10".to_string(),
            })
        );
    }
    Ok(())
}

#[test]
fn non_gpio_second_down_settles_an_expired_low_before_consuming_the_click() -> Result<()> {
    for delay in [219, 220, 221] {
        for target_power in [false, true] {
            let start = Instant::now();
            let mut model = projection_model()?;
            model.base_url = "http://127.0.0.1:9".to_string();
            model.timeout = Duration::from_millis(30);
            model.last_http_poll = Some(start);
            model.power_names.push("12v_out".to_string());
            model.power_states.insert("12v_out".to_string(), false);
            draw(&mut model, 80, 24)?;
            let gpio = control_rect(&model, "GP10")?;
            let target = if target_power {
                model
                    .hit_map
                    .controls
                    .iter()
                    .find(|(_, item)| *item == &ControlItem::Power("12v_out".to_string()))
                    .map(|(rect, _)| *rect)
                    .expect("power row hit target")
            } else {
                ratatui::layout::Rect::new(79, 23, 1, 1)
            };
            handle_mouse_at(
                &mut model,
                mouse(MouseEventKind::Down(MouseButton::Left), gpio.x, gpio.y),
                start,
            )?;
            handle_mouse_at(
                &mut model,
                mouse(MouseEventKind::Up(MouseButton::Left), gpio.x, gpio.y),
                start,
            )?;
            handle_mouse_at(
                &mut model,
                mouse(MouseEventKind::Down(MouseButton::Left), target.x, target.y),
                start + Duration::from_millis(delay),
            )?;

            if delay < 220 {
                assert_eq!(model.gpio_pending, None, "target_power={target_power}");
            } else {
                assert_eq!(
                    model.gpio_pending,
                    Some(GpioJob {
                        action: GpioAction::DriveLow,
                        target: "GP10".to_string(),
                    }),
                    "delay={delay} target_power={target_power}"
                );
            }
            assert!(model.hardware_confirm.is_none());
        }
    }
    Ok(())
}
