use super::gpio_gesture_types::GpioGestureState;
pub(super) use super::gpio_gesture_types::{
    GpioGesture, GpioGestureAction, GpioGestureInput, LONG_PRESS_THRESHOLD, SHORT_PRESS_WINDOW,
};
use super::gpio_io::GpioAction;
use std::time::Instant;

impl GpioGesture {
    pub(super) fn down(
        &mut self,
        input: GpioGestureInput<'_>,
        now: Instant,
    ) -> Option<GpioGestureAction> {
        let state = std::mem::replace(&mut self.state, GpioGestureState::Idle);
        let (next, action) = match state {
            GpioGestureState::Idle => (Self::down_state(input, now, false), None),
            GpioGestureState::Down {
                pin, hold_deadline, ..
            } if now >= hold_deadline => (
                GpioGestureState::HoldFired,
                Some(GpioGestureAction {
                    pin,
                    action: GpioAction::DriveHigh,
                }),
            ),
            GpioGestureState::Down {
                pin,
                origin,
                hold_deadline,
                second,
            } => match input.pin {
                Some(next_pin) if next_pin == pin && input.origin() == origin => (
                    GpioGestureState::Down {
                        pin,
                        origin,
                        hold_deadline,
                        second,
                    },
                    None,
                ),
                Some(_) | None => (GpioGestureState::Idle, None),
            },
            GpioGestureState::AwaitSecond {
                pin, low_deadline, ..
            } if now >= low_deadline => (
                GpioGestureState::Idle,
                Some(GpioGestureAction {
                    pin,
                    action: GpioAction::DriveLow,
                }),
            ),
            GpioGestureState::AwaitSecond { pin, origin, .. } => match input.pin {
                Some(next_pin) if next_pin == pin && input.origin() == origin => {
                    (Self::down_state(input, now, true), None)
                }
                Some(_) | None => (GpioGestureState::Idle, None),
            },
            GpioGestureState::HoldFired => (GpioGestureState::HoldFired, None),
        };
        self.state = next;
        action
    }

    pub(super) fn up(
        &mut self,
        input: GpioGestureInput<'_>,
        now: Instant,
    ) -> Option<GpioGestureAction> {
        let state = std::mem::replace(&mut self.state, GpioGestureState::Idle);
        let (next, action) = match state {
            GpioGestureState::Idle => (GpioGestureState::Idle, None),
            GpioGestureState::Down {
                pin,
                origin,
                hold_deadline,
                second,
            } => match input.pin {
                Some(next_pin) if next_pin == pin && input.origin() == origin => {
                    if now >= hold_deadline {
                        (
                            GpioGestureState::Idle,
                            Some(GpioGestureAction {
                                pin,
                                action: GpioAction::DriveHigh,
                            }),
                        )
                    } else if second {
                        (
                            GpioGestureState::Idle,
                            Some(GpioGestureAction {
                                pin,
                                action: GpioAction::SetInput,
                            }),
                        )
                    } else {
                        (
                            GpioGestureState::AwaitSecond {
                                pin,
                                origin,
                                low_deadline: now + SHORT_PRESS_WINDOW,
                            },
                            None,
                        )
                    }
                }
                Some(_) | None => (GpioGestureState::Idle, None),
            },
            GpioGestureState::AwaitSecond {
                pin,
                origin,
                low_deadline,
            } => (
                GpioGestureState::AwaitSecond {
                    pin,
                    origin,
                    low_deadline,
                },
                None,
            ),
            GpioGestureState::HoldFired => (GpioGestureState::Idle, None),
        };
        self.state = next;
        action
    }

    pub(super) fn tick(&mut self, now: Instant) -> Option<GpioGestureAction> {
        let state = std::mem::replace(&mut self.state, GpioGestureState::Idle);
        let (next, action) = match state {
            GpioGestureState::Idle => (GpioGestureState::Idle, None),
            GpioGestureState::Down {
                pin,
                origin,
                hold_deadline,
                second,
            } => {
                if now >= hold_deadline {
                    (
                        GpioGestureState::HoldFired,
                        Some(GpioGestureAction {
                            pin,
                            action: GpioAction::DriveHigh,
                        }),
                    )
                } else {
                    (
                        GpioGestureState::Down {
                            pin,
                            origin,
                            hold_deadline,
                            second,
                        },
                        None,
                    )
                }
            }
            GpioGestureState::AwaitSecond {
                pin,
                origin,
                low_deadline,
            } => {
                if now >= low_deadline {
                    (
                        GpioGestureState::Idle,
                        Some(GpioGestureAction {
                            pin,
                            action: GpioAction::DriveLow,
                        }),
                    )
                } else {
                    (
                        GpioGestureState::AwaitSecond {
                            pin,
                            origin,
                            low_deadline,
                        },
                        None,
                    )
                }
            }
            GpioGestureState::HoldFired => (GpioGestureState::HoldFired, None),
        };
        self.state = next;
        action
    }

    fn down_state(input: GpioGestureInput<'_>, now: Instant, second: bool) -> GpioGestureState {
        match input.pin {
            Some(pin) => GpioGestureState::Down {
                pin: pin.to_string(),
                origin: input.origin(),
                hold_deadline: now + LONG_PRESS_THRESHOLD,
                second,
            },
            None => GpioGestureState::Idle,
        }
    }
}
