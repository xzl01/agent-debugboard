use super::actions::ControlIntent;
use crossterm::event::{KeyCode, KeyModifiers};

pub(super) fn decode_direct_gpio_intent(
    code: KeyCode,
    modifiers: KeyModifiers,
) -> Option<ControlIntent> {
    match (code, modifiers) {
        (KeyCode::Char('l'), KeyModifiers::NONE) | (KeyCode::Char('L'), KeyModifiers::SHIFT) => {
            Some(ControlIntent::DriveLow)
        }
        (KeyCode::Char('o'), KeyModifiers::NONE) | (KeyCode::Char('O'), KeyModifiers::SHIFT) => {
            Some(ControlIntent::DriveHigh)
        }
        (KeyCode::Char('i'), KeyModifiers::NONE) | (KeyCode::Char('I'), KeyModifiers::SHIFT) => {
            Some(ControlIntent::SetInput)
        }
        _ => None,
    }
}
