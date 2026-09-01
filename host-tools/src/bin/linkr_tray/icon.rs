use anyhow::{Context, Result};
use tray_icon::Icon;

use super::ServiceState;

// allow: SIZE_OK — four SVG-derived arm-mask tables dominate this renderer module.

const ICON_SIDE: usize = 32;
const ICON_SIZE: u32 = 32;
const TRANSPARENT: [u8; 4] = [0, 0, 0, 0];
const WHITE: [u8; 4] = [255, 255, 255, 255];
const STARTING_BACKGROUND: [u8; 4] = [245, 158, 11, 255];
const READY_BACKGROUND: [u8; 4] = [30, 41, 59, 255];
const OFFLINE_BACKGROUND: [u8; 4] = [239, 68, 68, 255];
const IDENTITY_GREEN: [u8; 4] = [116, 188, 31, 255];
const RAIL_OFF: [u8; 4] = [100, 116, 139, 255];
const UART_ACTIVE: [u8; 4] = [96, 165, 250, 255];
const UART_IDLE: [u8; 4] = [48, 83, 126, 255];
const HEARTBEAT_ACTIVE: [u8; 4] = [52, 211, 153, 255];
const HEARTBEAT_IDLE: [u8; 4] = [24, 95, 69, 255];

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub(super) struct IndicatorState {
    rails: [bool; 3],
    uart_bridge_active: bool,
    logic_analyzer_active: bool,
    board_online: bool,
}

impl IndicatorState {
    pub(super) const fn new(
        rails: [bool; 3],
        uart_bridge_active: bool,
        logic_analyzer_active: bool,
    ) -> Self {
        Self {
            rails,
            uart_bridge_active,
            logic_analyzer_active,
            board_online: true,
        }
    }

    pub(super) const fn logic_analyzer_active(self) -> bool {
        self.logic_analyzer_active
    }

    pub(super) const fn board_online(self) -> bool {
        self.board_online
    }

    pub(super) const fn with_host_activity(
        mut self,
        uart_bridge_active: bool,
        logic_analyzer_active: bool,
    ) -> Self {
        self.uart_bridge_active = uart_bridge_active;
        self.logic_analyzer_active = logic_analyzer_active;
        self
    }

    pub(super) const fn host_activity(self) -> (bool, bool) {
        (self.uart_bridge_active, self.logic_analyzer_active)
    }
}

#[derive(Clone, Copy)]
pub(super) struct IconFrame {
    pub(super) state: ServiceState,
    pub(super) animation: u8,
    pub(super) indicator: IndicatorState,
}

pub(super) fn status_icon(frame: IconFrame) -> Result<Icon> {
    Icon::from_rgba(status_rgba(frame), ICON_SIZE, ICON_SIZE).context("build tray status icon")
}

pub(super) fn changed_status_icon(
    frame: IconFrame,
    published_rgba: &mut Vec<u8>,
) -> Result<Option<Icon>> {
    let rgba = status_rgba(frame);
    if rgba == *published_rgba {
        return Ok(None);
    }
    let icon =
        Icon::from_rgba(rgba.clone(), ICON_SIZE, ICON_SIZE).context("build tray status icon")?;
    *published_rgba = rgba;
    Ok(Some(icon))
}

pub(super) fn status_rgba(frame: IconFrame) -> Vec<u8> {
    let mut rgba = vec![0_u8; ICON_SIDE * ICON_SIDE * 4];
    for y in 0..ICON_SIDE {
        for x in 0..ICON_SIDE {
            let pixel = (y * ICON_SIDE + x) * 4;
            let value = status_pixel(frame, x, y);
            rgba[pixel..pixel + 4].copy_from_slice(&value);
        }
    }
    rgba
}

const LINKR_ORIGINAL_X_ARM_MASKS: [[u32; ICON_SIDE]; 4] = [
    [
        0x00000000, 0x00000000, 0x00000000, 0x000003c0, 0x000003e0, 0x000007f0, 0x00000ff8,
        0x00001ffc, 0x00003ffc, 0x00003ff8, 0x00007fe0, 0x0000ffc0, 0x00007f80, 0x00003e00,
        0x00001c00, 0x00000800, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
    ],
    [
        0x00000000, 0x00000000, 0x00000000, 0x03800000, 0x07c00000, 0x0fe00000, 0x1ff00000,
        0x3ff80000, 0x3ff80000, 0x1ffc0000, 0x07fe0000, 0x03ff0000, 0x01fe0000, 0x00fc0000,
        0x00380000, 0x00100000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
    ],
    [
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00100000, 0x00380000, 0x00fc0000, 0x01fe0000, 0x03ff0000,
        0x07fe0000, 0x1ffc0000, 0x3ffc0000, 0x3ff80000, 0x1ff00000, 0x0fe00000, 0x07c00000,
        0x03c00000, 0x00000000, 0x00000000, 0x00000000,
    ],
    [
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000800, 0x00001c00, 0x00003e00, 0x00007f80, 0x0000ffc0,
        0x00007fe0, 0x00003ff8, 0x00003ffc, 0x00001ffc, 0x00000ff8, 0x000007f0, 0x000003e0,
        0x000003c0, 0x00000000, 0x00000000, 0x00000000,
    ],
];

fn linkr_original_x_arm(x: usize, y: usize) -> Option<u8> {
    if x >= ICON_SIDE || y >= ICON_SIDE {
        return None;
    }
    let bit = 1_u32 << x;
    if LINKR_ORIGINAL_X_ARM_MASKS[0][y] & bit != 0 {
        Some(0)
    } else if LINKR_ORIGINAL_X_ARM_MASKS[1][y] & bit != 0 {
        Some(1)
    } else if LINKR_ORIGINAL_X_ARM_MASKS[2][y] & bit != 0 {
        Some(2)
    } else if LINKR_ORIGINAL_X_ARM_MASKS[3][y] & bit != 0 {
        Some(3)
    } else {
        None
    }
}

fn tray_icon_background_pixel(x: usize, y: usize) -> bool {
    if x == 0 || x >= 31 || y == 0 || y >= 31 {
        return false;
    }
    let dx = if x < 7 { 7 - x } else { x.saturating_sub(24) };
    let dy = if y < 7 { 7 - y } else { y.saturating_sub(24) };
    dx * dx + dy * dy <= 36
}

fn status_pixel(frame: IconFrame, x: usize, y: usize) -> [u8; 4] {
    if !tray_icon_background_pixel(x, y) {
        return TRANSPARENT;
    }
    match frame.state {
        ServiceState::Starting => linkr_original_x_arm(x, y)
            .filter(|arm| *arm == frame.animation % 4)
            .map_or(STARTING_BACKGROUND, |_| WHITE),
        ServiceState::Offline => {
            if frame.animation.is_multiple_of(2) && linkr_original_x_arm(x, y).is_some() {
                WHITE
            } else {
                OFFLINE_BACKGROUND
            }
        }
        ServiceState::Ready => ready_pixel(frame, x, y),
    }
}

fn ready_pixel(frame: IconFrame, x: usize, y: usize) -> [u8; 4] {
    if center_heartbeat_pixel(x, y) {
        return if center_heartbeat_active(frame) {
            HEARTBEAT_ACTIVE
        } else {
            HEARTBEAT_IDLE
        };
    }
    if frame.indicator.logic_analyzer_active {
        return if rotated_mark_pixel(x, y, frame.animation) {
            IDENTITY_GREEN
        } else {
            READY_BACKGROUND
        };
    }
    match linkr_original_x_arm(x, y) {
        Some(0) => rail_color(frame.indicator.rails[0]),
        Some(1) => rail_color(frame.indicator.rails[1]),
        Some(2) => uart_color(frame),
        Some(3) => rail_color(frame.indicator.rails[2]),
        Some(_) | None => READY_BACKGROUND,
    }
}

const fn rail_color(enabled: bool) -> [u8; 4] {
    if enabled {
        IDENTITY_GREEN
    } else {
        RAIL_OFF
    }
}

fn uart_color(frame: IconFrame) -> [u8; 4] {
    if !frame.indicator.uart_bridge_active {
        return RAIL_OFF;
    }
    if frame.animation % 8 < 2 {
        UART_ACTIVE
    } else {
        UART_IDLE
    }
}

fn center_heartbeat_pixel(x: usize, y: usize) -> bool {
    (x * 2).abs_diff(31) + (y * 2).abs_diff(31) <= 8
}

fn center_heartbeat_active(frame: IconFrame) -> bool {
    if !frame.indicator.board_online {
        return false;
    }
    if frame.indicator.logic_analyzer_active {
        matches!(frame.animation % 14, 0..=2 | 5..=6)
    } else {
        matches!(frame.animation % 8, 0..=1 | 3)
    }
}

fn rotated_mark_pixel(x: usize, y: usize, animation: u8) -> bool {
    const ROTATION: [(i32, i32); 8] = [
        (1024, 0),
        (724, -724),
        (0, -1024),
        (-724, -724),
        (-1024, 0),
        (-724, 724),
        (0, 1024),
        (724, 724),
    ];
    let Ok(x) = i32::try_from(x) else {
        return false;
    };
    let Ok(y) = i32::try_from(y) else {
        return false;
    };
    let (cos, sin) = ROTATION[usize::from(animation % 8)];
    let dx = x * 2 - 31;
    let dy = y * 2 - 31;
    let source_dx = rounded_div(cos * dx - sin * dy, 1024);
    let source_dy = rounded_div(sin * dx + cos * dy, 1024);
    let source_x = rounded_div(source_dx + 31, 2);
    let source_y = rounded_div(source_dy + 31, 2);
    let (Ok(source_x), Ok(source_y)) = (usize::try_from(source_x), usize::try_from(source_y))
    else {
        return false;
    };
    linkr_original_x_arm(source_x, source_y).is_some()
}

const fn rounded_div(value: i32, divisor: i32) -> i32 {
    if value >= 0 {
        (value + divisor / 2) / divisor
    } else {
        (value - divisor / 2) / divisor
    }
}

#[cfg(test)]
mod tests {
    use super::{
        linkr_original_x_arm, status_rgba, tray_icon_background_pixel, IconFrame, IndicatorState,
        ServiceState,
    };

    fn pixel(rgba: &[u8], x: usize, y: usize) -> [u8; 4] {
        let offset = (y * 32 + x) * 4;
        rgba[offset..offset + 4].try_into().expect("RGBA pixel")
    }

    #[test]
    fn tray_mark_rasterizes_the_original_radxa_x() {
        assert!(linkr_original_x_arm(8, 8).is_some());
        assert!(linkr_original_x_arm(22, 8).is_some());
        assert!(linkr_original_x_arm(8, 22).is_some());
        assert!(linkr_original_x_arm(22, 22).is_some());
        assert!(linkr_original_x_arm(8, 3).is_some());
        assert!(linkr_original_x_arm(25, 3).is_some());
        assert!(linkr_original_x_arm(8, 28).is_some());
        assert!(linkr_original_x_arm(25, 28).is_some());
        assert!(linkr_original_x_arm(15, 15).is_none());
        assert!(linkr_original_x_arm(15, 0).is_none());
        assert!(tray_icon_background_pixel(8, 3));
        assert!(tray_icon_background_pixel(25, 3));
        assert!(tray_icon_background_pixel(8, 28));
        assert!(tray_icon_background_pixel(25, 28));
        assert!(!tray_icon_background_pixel(0, 0));
    }

    #[test]
    fn tray_ready_maps_rails_uart_heartbeat_center_and_neutral_backplate() {
        let endpoints = [(0_u8, 8, 8), (1, 22, 8), (2, 22, 22), (3, 8, 22)];
        let rgba = status_rgba(IconFrame {
            state: ServiceState::Ready,
            animation: 0,
            indicator: IndicatorState::new([true, false, true], true, false),
        });

        assert_eq!(
            pixel(&rgba, endpoints[0].1, endpoints[0].2),
            [116, 188, 31, 255]
        );
        assert_eq!(
            pixel(&rgba, endpoints[1].1, endpoints[1].2),
            [100, 116, 139, 255]
        );
        assert_eq!(
            pixel(&rgba, endpoints[2].1, endpoints[2].2),
            [96, 165, 250, 255]
        );
        assert_eq!(
            pixel(&rgba, endpoints[3].1, endpoints[3].2),
            [116, 188, 31, 255]
        );
        assert_eq!(pixel(&rgba, 15, 15), [52, 211, 153, 255]);
        assert_eq!(pixel(&rgba, 15, 1), [30, 41, 59, 255]);
        assert_eq!(pixel(&rgba, 0, 0), [0, 0, 0, 0]);
    }

    #[test]
    fn tray_center_heartbeat_requires_a_fresh_board_snapshot() {
        let offline_board = status_rgba(IconFrame {
            state: ServiceState::Ready,
            animation: 0,
            indicator: IndicatorState::default(),
        });
        let online_board = status_rgba(IconFrame {
            state: ServiceState::Ready,
            animation: 0,
            indicator: IndicatorState::new([false, false, false], false, false),
        });

        assert_eq!(pixel(&offline_board, 15, 15), [24, 95, 69, 255]);
        assert_eq!(pixel(&online_board, 15, 15), [52, 211, 153, 255]);
    }

    #[test]
    fn faster_center_heartbeat_preserves_the_uart_period() {
        let uart = IndicatorState::new([false, false, false], true, false);
        for animation in 0..8 {
            let frame = IconFrame {
                state: ServiceState::Ready,
                animation,
                indicator: uart,
            };
            let expected = if animation < 2 {
                super::UART_ACTIVE
            } else {
                super::UART_IDLE
            };
            assert_eq!(super::uart_color(frame), expected);
        }

        let logic = IndicatorState::new([false, false, false], false, true);
        let frame = |animation| IconFrame {
            state: ServiceState::Ready,
            animation,
            indicator: logic,
        };
        assert!(super::center_heartbeat_active(frame(0)));
        assert!(!super::center_heartbeat_active(frame(13)));
        assert!(super::center_heartbeat_active(frame(14)));
    }

    #[test]
    fn tray_ready_skips_identical_frames_and_only_changes_the_center_heartbeat() {
        let indicator = IndicatorState::new([true, false, true], false, false);
        let first = IconFrame {
            state: ServiceState::Ready,
            animation: 0,
            indicator,
        };
        let identical = IconFrame {
            animation: 1,
            ..first
        };
        let heartbeat_idle = IconFrame {
            animation: 2,
            ..first
        };
        let mut published = status_rgba(first);

        assert!(super::changed_status_icon(identical, &mut published)
            .expect("compare identical tray frame")
            .is_none());
        let before_heartbeat = published.clone();
        assert!(super::changed_status_icon(heartbeat_idle, &mut published)
            .expect("build changed tray frame")
            .is_some());
        for (pixel_index, (before, after)) in before_heartbeat
            .chunks_exact(4)
            .zip(published.chunks_exact(4))
            .enumerate()
        {
            if before != after {
                assert!(super::center_heartbeat_pixel(
                    pixel_index % 32,
                    pixel_index / 32
                ));
            }
        }
    }

    #[test]
    fn tray_logic_analyzer_rotates_the_original_mark() {
        let indicator = IndicatorState::new([true, true, true], true, true);
        let first = status_rgba(IconFrame {
            state: ServiceState::Ready,
            animation: 0,
            indicator,
        });
        let next = status_rgba(IconFrame {
            state: ServiceState::Ready,
            animation: 1,
            indicator,
        });

        assert_ne!(first, next);
        assert_eq!(pixel(&first, 15, 15), [52, 211, 153, 255]);
        assert_eq!(pixel(&next, 15, 15), [52, 211, 153, 255]);
    }

    #[test]
    fn tray_mark_frames_chase_starting_and_blink_offline() {
        let endpoints = [(0_u8, 8, 8), (1, 22, 8), (2, 22, 22), (3, 8, 22)];
        let indicator = IndicatorState::default();

        for (animation, _, _) in endpoints {
            let starting = status_rgba(IconFrame {
                state: ServiceState::Starting,
                animation,
                indicator,
            });
            for (arm, x, y) in endpoints {
                let expected = if arm == animation {
                    [255, 255, 255, 255]
                } else {
                    [245, 158, 11, 255]
                };
                assert_eq!(pixel(&starting, x, y), expected);
            }
        }

        let offline_on = status_rgba(IconFrame {
            state: ServiceState::Offline,
            animation: 0,
            indicator,
        });
        let offline_off = status_rgba(IconFrame {
            state: ServiceState::Offline,
            animation: 1,
            indicator,
        });
        for (_, x, y) in endpoints {
            assert_eq!(pixel(&offline_on, x, y), [255, 255, 255, 255]);
            assert_eq!(pixel(&offline_off, x, y), [239, 68, 68, 255]);
        }
    }
}
