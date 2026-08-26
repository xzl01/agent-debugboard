use super::gpio_io::GpioAction;
use std::time::{Duration, Instant};

pub(super) const SHORT_PRESS_WINDOW: Duration = Duration::from_millis(220);
pub(super) const LONG_PRESS_THRESHOLD: Duration = Duration::from_millis(600);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct GpioGestureInput<'a> {
    pub(super) pin: Option<&'a str>,
    pub(super) column: u16,
    pub(super) row: u16,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct GpioGestureOrigin {
    column: u16,
    row: u16,
}

impl<'a> GpioGestureInput<'a> {
    pub(super) const fn origin(self) -> GpioGestureOrigin {
        GpioGestureOrigin {
            column: self.column,
            row: self.row,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct GpioGestureAction {
    pub(super) pin: String,
    pub(super) action: GpioAction,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum GpioGestureState {
    Idle,
    Down {
        pin: String,
        origin: GpioGestureOrigin,
        hold_deadline: Instant,
        second: bool,
    },
    AwaitSecond {
        pin: String,
        origin: GpioGestureOrigin,
        low_deadline: Instant,
    },
    HoldFired,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct GpioGesture {
    pub(super) state: GpioGestureState,
}

impl Default for GpioGesture {
    fn default() -> Self {
        Self {
            state: GpioGestureState::Idle,
        }
    }
}
