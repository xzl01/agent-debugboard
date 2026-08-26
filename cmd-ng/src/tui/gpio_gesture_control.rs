use super::gpio_gesture_types::{GpioGesture, GpioGestureInput, GpioGestureState};

impl GpioGesture {
    pub(super) fn moved(&mut self, input: GpioGestureInput<'_>) {
        let state = std::mem::replace(&mut self.state, GpioGestureState::Idle);
        self.state = match state {
            GpioGestureState::Idle => GpioGestureState::Idle,
            GpioGestureState::Down {
                pin,
                origin,
                hold_deadline,
                second,
            } => match input.pin {
                Some(next_pin) if next_pin == pin && input.origin() == origin => {
                    GpioGestureState::Down {
                        pin,
                        origin,
                        hold_deadline,
                        second,
                    }
                }
                Some(_) | None => GpioGestureState::Idle,
            },
            GpioGestureState::AwaitSecond {
                pin,
                origin,
                low_deadline,
            } => match input.pin {
                Some(next_pin) if next_pin == pin && input.origin() == origin => {
                    GpioGestureState::AwaitSecond {
                        pin,
                        origin,
                        low_deadline,
                    }
                }
                Some(_) | None => GpioGestureState::Idle,
            },
            GpioGestureState::HoldFired => GpioGestureState::Idle,
        };
    }

    pub(super) fn holding_pin(&self) -> Option<&str> {
        match &self.state {
            GpioGestureState::Down { pin, .. } => Some(pin),
            GpioGestureState::Idle
            | GpioGestureState::AwaitSecond { .. }
            | GpioGestureState::HoldFired => None,
        }
    }

    pub(super) fn is_active(&self) -> bool {
        !matches!(self.state, GpioGestureState::Idle)
    }

    pub(super) fn cancel(&mut self) {
        self.state = GpioGestureState::Idle;
    }

    pub(super) fn cancel_missing_pin(&mut self, gpio_names: &[String]) {
        let tracked = match &self.state {
            GpioGestureState::Down { pin, .. } | GpioGestureState::AwaitSecond { pin, .. } => {
                Some(pin.as_str())
            }
            GpioGestureState::Idle | GpioGestureState::HoldFired => None,
        };
        if tracked.is_some_and(|pin| !gpio_names.iter().any(|name| name == pin)) {
            self.cancel();
        }
    }
}
