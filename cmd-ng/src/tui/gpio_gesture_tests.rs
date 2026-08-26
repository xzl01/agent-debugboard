use super::gpio_gesture::{GpioGesture, GpioGestureAction, GpioGestureInput};
use super::gpio_io::GpioAction;
use std::time::{Duration, Instant};

fn input(pin: Option<&str>, column: u16, row: u16) -> GpioGestureInput<'_> {
    GpioGestureInput { pin, column, row }
}

fn action(pin: &str, action: GpioAction) -> GpioGestureAction {
    GpioGestureAction {
        pin: pin.to_string(),
        action,
    }
}

#[test]
fn short_press_dispatches_low_at_the_exact_220ms_deadline() {
    let start = Instant::now();
    let mut gesture = GpioGesture::default();
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    assert_eq!(gesture.up(input(Some("GP10"), 4, 8), start), None);
    assert_eq!(gesture.tick(start + Duration::from_millis(219)), None);
    assert_eq!(
        gesture.tick(start + Duration::from_millis(220)),
        Some(action("GP10", GpioAction::DriveLow))
    );
}

#[test]
fn short_press_emits_the_gpio_name_with_its_action() {
    let start = Instant::now();
    let mut gesture = GpioGesture::default();
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    gesture.up(input(Some("GP10"), 4, 8), start);
    assert_eq!(
        gesture.tick(start + Duration::from_millis(220)),
        Some(action("GP10", GpioAction::DriveLow))
    );
}

#[test]
fn hold_dispatches_high_once_at_the_exact_600ms_deadline() {
    let start = Instant::now();
    let mut gesture = GpioGesture::default();
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    assert_eq!(gesture.tick(start + Duration::from_millis(599)), None);
    assert_eq!(
        gesture.tick(start + Duration::from_millis(600)),
        Some(action("GP10", GpioAction::DriveHigh))
    );
    assert_eq!(gesture.tick(start + Duration::from_millis(601)), None);
}

#[test]
fn up_at_599ms_enters_await_second_before_tick() {
    let start = Instant::now();
    let mut gesture = GpioGesture::default();
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    assert_eq!(
        gesture.up(
            input(Some("GP10"), 4, 8),
            start + Duration::from_millis(599)
        ),
        None
    );
    assert_eq!(gesture.tick(start + Duration::from_millis(818)), None);
    assert_eq!(
        gesture.tick(start + Duration::from_millis(819)),
        Some(action("GP10", GpioAction::DriveLow))
    );
}

#[test]
fn up_at_600ms_dispatches_high_before_tick() {
    let start = Instant::now();
    let mut gesture = GpioGesture::default();
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    assert_eq!(
        gesture.up(
            input(Some("GP10"), 4, 8),
            start + Duration::from_millis(600)
        ),
        Some(action("GP10", GpioAction::DriveHigh))
    );
    assert_eq!(gesture.tick(start + Duration::from_millis(601)), None);
}

#[test]
fn up_after_600ms_dispatches_high_before_tick() {
    let start = Instant::now();
    let mut gesture = GpioGesture::default();
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    assert_eq!(
        gesture.up(
            input(Some("GP10"), 4, 8),
            start + Duration::from_millis(601)
        ),
        Some(action("GP10", GpioAction::DriveHigh))
    );
    assert_eq!(gesture.tick(start + Duration::from_millis(602)), None);
}

#[test]
fn up_after_a_fired_hold_is_inert_and_returns_to_idle() {
    let start = Instant::now();
    let mut gesture = GpioGesture::default();
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    assert_eq!(
        gesture.tick(start + Duration::from_millis(600)),
        Some(action("GP10", GpioAction::DriveHigh))
    );
    assert_eq!(gesture.up(input(Some("GP10"), 4, 8), start), None);
    assert_eq!(gesture.holding_pin(), None);
}

#[test]
fn same_pin_double_press_dispatches_input_without_a_transient_low() {
    let start = Instant::now();
    let mut gesture = GpioGesture::default();
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    assert_eq!(gesture.up(input(Some("GP10"), 4, 8), start), None);
    let _ = gesture.down(
        input(Some("GP10"), 4, 8),
        start + Duration::from_millis(100),
    );
    assert_eq!(
        gesture.up(
            input(Some("GP10"), 4, 8),
            start + Duration::from_millis(101)
        ),
        Some(action("GP10", GpioAction::SetInput))
    );
    assert_eq!(gesture.tick(start + Duration::from_millis(220)), None);
}

#[test]
fn second_down_at_219ms_is_a_double_press_candidate() {
    let start = Instant::now();
    let mut gesture = GpioGesture::default();
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    assert_eq!(gesture.up(input(Some("GP10"), 4, 8), start), None);
    let _ = gesture.down(
        input(Some("GP10"), 4, 8),
        start + Duration::from_millis(219),
    );
    assert_eq!(
        gesture.up(
            input(Some("GP10"), 4, 8),
            start + Duration::from_millis(219)
        ),
        Some(action("GP10", GpioAction::SetInput))
    );
}

#[test]
fn expired_second_down_returns_low_then_allows_a_fresh_gesture() {
    let start = Instant::now();
    let mut gesture = GpioGesture::default();
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    assert_eq!(gesture.up(input(Some("GP10"), 4, 8), start), None);
    assert_eq!(
        gesture.down(
            input(Some("GP10"), 4, 8),
            start + Duration::from_millis(220)
        ),
        Some(action("GP10", GpioAction::DriveLow))
    );
    assert_eq!(
        gesture.up(
            input(Some("GP10"), 4, 8),
            start + Duration::from_millis(221)
        ),
        None
    );
    assert_eq!(
        gesture.down(
            input(Some("GP10"), 4, 8),
            start + Duration::from_millis(222)
        ),
        None
    );
    assert_eq!(
        gesture.up(
            input(Some("GP10"), 4, 8),
            start + Duration::from_millis(222)
        ),
        None
    );
    assert_eq!(
        gesture.tick(start + Duration::from_millis(442)),
        Some(action("GP10", GpioAction::DriveLow))
    );
}

#[test]
fn a_different_pin_second_down_cancels_and_is_consumed() {
    let start = Instant::now();
    let mut gesture = GpioGesture::default();
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    gesture.up(input(Some("GP10"), 4, 8), start);
    let _ = gesture.down(input(Some("GP11"), 5, 8), start + Duration::from_millis(1));
    assert_eq!(gesture.holding_pin(), None);
    assert_eq!(gesture.tick(start + Duration::from_millis(220)), None);
}

#[test]
fn a_non_gpio_second_down_cancels_and_is_consumed() {
    let start = Instant::now();
    let mut gesture = GpioGesture::default();
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    gesture.up(input(Some("GP10"), 4, 8), start);
    let _ = gesture.down(input(None, 0, 0), start + Duration::from_millis(1));
    assert_eq!(gesture.holding_pin(), None);
    assert_eq!(gesture.tick(start + Duration::from_millis(220)), None);
}

#[test]
fn a_coordinate_change_cancels_a_held_press() {
    let start = Instant::now();
    let mut gesture = GpioGesture::default();
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    gesture.moved(input(Some("GP10"), 5, 8));
    assert_eq!(gesture.holding_pin(), None);
    assert_eq!(gesture.tick(start + Duration::from_millis(600)), None);
}

#[test]
fn holding_pin_tracks_only_an_active_down_state() {
    let start = Instant::now();
    let mut gesture = GpioGesture::default();
    assert_eq!(gesture.holding_pin(), None);
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    assert_eq!(gesture.holding_pin(), Some("GP10"));
    gesture.up(input(Some("GP10"), 4, 8), start);
    assert_eq!(gesture.holding_pin(), None);
}

#[test]
fn explicit_and_missing_pin_cancellation_clear_the_gesture() {
    let start = Instant::now();
    let mut gesture = GpioGesture::default();
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    gesture.cancel();
    assert_eq!(gesture.holding_pin(), None);
    let _ = gesture.down(input(Some("GP10"), 4, 8), start);
    gesture.cancel_missing_pin(&["GP11".to_string()]);
    assert_eq!(gesture.holding_pin(), None);
    assert_eq!(gesture.tick(start + Duration::from_millis(600)), None);
}
