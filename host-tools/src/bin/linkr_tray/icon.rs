use anyhow::Result;
use std::time::{Duration, Instant};

use super::ServiceState;

// allow: SIZE_OK — four SVG-derived arm-mask tables dominate this renderer module.

const ICON_SIDE: usize = 32;
const ICON_RETRY_INTERVAL: Duration = Duration::from_secs(1);
const TRANSPARENT: [u8; 4] = [0, 0, 0, 0];
const WHITE: [u8; 4] = [255, 255, 255, 255];
const STARTING_BACKGROUND: [u8; 4] = [245, 158, 11, 255];
const READY_BACKGROUND: [u8; 4] = [30, 41, 59, 255];
const OFFLINE_BACKGROUND: [u8; 4] = [239, 68, 68, 255];
const IDENTITY_GREEN: [u8; 4] = [116, 188, 31, 255];
const RAIL_OFF: [u8; 4] = [100, 116, 139, 255];
const ONLINE_ACTIVE: [u8; 4] = [52, 211, 153, 255];
const ONLINE_IDLE: [u8; 4] = [24, 95, 69, 255];

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub(super) struct IndicatorState {
    // Original X arm order: top-left 5V, top-right 12V, bottom-right VDD5V,
    // bottom-left 20V. Match the Web UI's four power indicators.
    rails: [bool; 4],
    logic_analyzer_active: bool,
    board_online: bool,
}

impl IndicatorState {
    pub(super) const fn new(rails: [bool; 4], logic_analyzer_active: bool) -> Self {
        Self {
            rails,
            logic_analyzer_active,
            board_online: true,
        }
    }

    pub(super) const fn board_online(self) -> bool {
        self.board_online
    }

    pub(super) const fn with_host_activity(mut self, logic_analyzer_active: bool) -> Self {
        self.logic_analyzer_active = logic_analyzer_active;
        self
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct IconFrame {
    pub(super) state: ServiceState,
    pub(super) indicator: IndicatorState,
    pub(super) heartbeat_on: bool,
}

impl IconFrame {
    fn effective(mut self) -> Self {
        match self.state {
            ServiceState::Starting | ServiceState::Offline => {
                self.indicator = IndicatorState::default();
                self.heartbeat_on = false;
            }
            ServiceState::Ready => {}
        }
        if !self.indicator.board_online {
            self.heartbeat_on = false;
        }
        self
    }

    pub(super) fn publication_key(self) -> u16 {
        let frame = self.effective();
        let indicator = frame.indicator;
        let state = match frame.state {
            ServiceState::Starting => 0,
            ServiceState::Offline => 1,
            ServiceState::Ready => 2,
        };
        state
            | (u16::from(indicator.rails[0]) << 2)
            | (u16::from(indicator.rails[1]) << 3)
            | (u16::from(indicator.rails[2]) << 4)
            | (u16::from(indicator.rails[3]) << 5)
            | (u16::from(indicator.logic_analyzer_active) << 6)
            | (u16::from(indicator.board_online) << 7)
            | (u16::from(frame.heartbeat_on) << 8)
    }
}

// Keep one successful frame and one failed-frame deadline, not an expanding cache.
pub(super) struct IconCache {
    frame: IconFrame,
    rgba: Vec<u8>,
    failed: Option<(u16, Instant)>,
}

impl IconCache {
    pub(super) fn new(frame: IconFrame) -> Self {
        let frame = frame.effective();
        Self {
            frame,
            rgba: status_rgba(frame),
            failed: None,
        }
    }

    pub(super) fn publish(
        &mut self,
        frame: IconFrame,
        publish: impl FnMut(u16, &[u8]) -> Result<()>,
    ) -> Result<bool> {
        self.publish_at(frame, Instant::now(), publish)
    }

    fn publish_at(
        &mut self,
        frame: IconFrame,
        now: Instant,
        mut publish: impl FnMut(u16, &[u8]) -> Result<()>,
    ) -> Result<bool> {
        let frame = frame.effective();
        let key = frame.publication_key();
        // Heartbeat edges do not constitute a new underlying visual/health state.
        let base_key = key & !0x100;
        if self.failed.is_some_and(|(failed, _)| failed != base_key) {
            self.failed = None;
        }
        if frame == self.frame {
            // Returning to the last successful pulse phase must not clear backoff.
            return Ok(false);
        }
        if self.failed.is_some_and(|(_, retry_at)| now < retry_at) {
            // Repeated probe events must not rerender/retry a failed static icon.
            return Ok(false);
        }
        let rgba = status_rgba(frame);
        if rgba == self.rgba {
            self.frame = frame;
            return Ok(false);
        }
        if let Err(error) = publish(key, &rgba) {
            self.failed = Some((base_key, now + ICON_RETRY_INTERVAL));
            return Err(error);
        }
        self.frame = frame;
        self.rgba = rgba;
        self.failed = None;
        Ok(true)
    }
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
        ServiceState::Starting => linkr_original_x_arm(x, y).map_or(STARTING_BACKGROUND, |_| WHITE),
        ServiceState::Offline => {
            if linkr_original_x_arm(x, y).is_some() {
                WHITE
            } else {
                OFFLINE_BACKGROUND
            }
        }
        ServiceState::Ready => ready_pixel(frame, x, y),
    }
}

fn ready_pixel(frame: IconFrame, x: usize, y: usize) -> [u8; 4] {
    if center_online_pixel(x, y) {
        return if frame.indicator.board_online && frame.heartbeat_on {
            ONLINE_ACTIVE
        } else {
            ONLINE_IDLE
        };
    }
    if let Some(arm) = linkr_original_x_arm(x, y) {
        return rail_color(frame.indicator.rails[usize::from(arm)]);
    }
    if frame.indicator.logic_analyzer_active && capture_border_pixel(x, y) {
        IDENTITY_GREEN
    } else {
        READY_BACKGROUND
    }
}

const fn rail_color(enabled: bool) -> [u8; 4] {
    if enabled {
        IDENTITY_GREEN
    } else {
        RAIL_OFF
    }
}

fn capture_border_pixel(x: usize, y: usize) -> bool {
    tray_icon_background_pixel(x, y)
        && (!tray_icon_background_pixel(x.saturating_sub(1), y)
            || !tray_icon_background_pixel(x + 1, y)
            || !tray_icon_background_pixel(x, y.saturating_sub(1))
            || !tray_icon_background_pixel(x, y + 1))
}

fn center_online_pixel(x: usize, y: usize) -> bool {
    (x * 2).abs_diff(31) + (y * 2).abs_diff(31) <= 8
}

#[cfg(test)]
mod tests {
    use std::time::{Duration, Instant};

    use super::{
        linkr_original_x_arm, status_rgba, tray_icon_background_pixel, IconFrame, IndicatorState,
        ServiceState,
    };

    fn pixel(rgba: &[u8], x: usize, y: usize) -> [u8; 4] {
        let offset = (y * 32 + x) * 4;
        rgba[offset..offset + 4].try_into().expect("RGBA pixel")
    }

    fn rails(bits: u8) -> [bool; 4] {
        std::array::from_fn(|arm| bits & (1 << arm) != 0)
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
    fn all_sixteen_power_combinations_map_to_the_four_webui_arms() {
        // Top-left 5V, top-right 12V, bottom-right VDD5V, bottom-left 20V.
        let endpoints = [(8, 8), (22, 8), (22, 22), (8, 22)];
        for bits in 0..16 {
            for capture in [false, true] {
                for heartbeat_on in [false, true] {
                    let indicator = IndicatorState::new(rails(bits), capture);
                    let rgba = status_rgba(IconFrame {
                        state: ServiceState::Ready,
                        indicator,
                        heartbeat_on,
                    });
                    for (arm, (x, y)) in endpoints.into_iter().enumerate() {
                        let expected = if indicator.rails[arm] {
                            super::IDENTITY_GREEN
                        } else {
                            super::RAIL_OFF
                        };
                        assert_eq!(pixel(&rgba, x, y), expected, "power {bits:04b}, arm {arm}");
                    }
                    assert_eq!(
                        pixel(&rgba, 15, 15),
                        if heartbeat_on {
                            super::ONLINE_ACTIVE
                        } else {
                            super::ONLINE_IDLE
                        }
                    );
                    assert_eq!(pixel(&rgba, 15, 2), super::READY_BACKGROUND);
                    assert_eq!(pixel(&rgba, 0, 0), super::TRANSPARENT);
                }
            }
        }
    }

    #[test]
    fn tray_center_online_indicator_requires_a_fresh_board_snapshot() {
        let offline_board = status_rgba(IconFrame {
            state: ServiceState::Ready,
            indicator: IndicatorState::default(),
            heartbeat_on: true,
        });
        let online_board = status_rgba(IconFrame {
            state: ServiceState::Ready,
            indicator: IndicatorState::new([false, false, false, false], false),
            heartbeat_on: true,
        });

        assert_eq!(pixel(&offline_board, 15, 15), [24, 95, 69, 255]);
        assert_eq!(pixel(&online_board, 15, 15), [52, 211, 153, 255]);
    }

    #[test]
    fn power_and_capture_preserve_both_center_pulse_phases() {
        for logic in [false, true] {
            for heartbeat_on in [false, true] {
                let frame = IconFrame {
                    state: ServiceState::Ready,
                    indicator: IndicatorState::new([false; 4], logic),
                    heartbeat_on,
                };
                let expected = if heartbeat_on {
                    super::ONLINE_ACTIVE
                } else {
                    super::ONLINE_IDLE
                };
                assert_eq!(pixel(&status_rgba(frame), 15, 15), expected);
            }
        }
    }

    #[test]
    fn tray_ready_skips_identical_state_but_publishes_real_power_changes() {
        let indicator = IndicatorState::new([true, false, false, true], false);
        let first = IconFrame {
            state: ServiceState::Ready,
            indicator,
            heartbeat_on: true,
        };
        let mut cache = super::IconCache::new(first);
        for _ in 0..10 {
            assert_eq!(status_rgba(first), cache.rgba);
            assert!(!cache
                .publish(first, |_, _| panic!("same phase must not publish"))
                .unwrap());
        }
        let before = cache.rgba.clone();
        let power_changed = IconFrame {
            indicator: IndicatorState::new([true, true, false, true], false),
            ..first
        };
        assert!(cache.publish(power_changed, |_, _| Ok(())).unwrap());
        assert_ne!(before, cache.rgba);
        for (pixel_index, (before, after)) in before
            .as_chunks::<4>()
            .0
            .iter()
            .zip(cache.rgba.as_chunks::<4>().0)
            .enumerate()
        {
            if before != after {
                assert_eq!(
                    linkr_original_x_arm(pixel_index % 32, pixel_index / 32),
                    Some(1)
                );
            }
        }
    }

    #[test]
    fn capture_only_changes_the_background_border_never_power_leaves_or_center() {
        for bits in 0..16 {
            for heartbeat_on in [false, true] {
                let idle = IconFrame {
                    state: ServiceState::Ready,
                    indicator: IndicatorState::new(rails(bits), false),
                    heartbeat_on,
                };
                let capture = IconFrame {
                    indicator: idle.indicator.with_host_activity(true),
                    ..idle
                };
                let idle_rgba = status_rgba(idle);
                let capture_rgba = status_rgba(capture);
                let mut changed = 0;
                for y in 0..32 {
                    for x in 0..32 {
                        let before = pixel(&idle_rgba, x, y);
                        let after = pixel(&capture_rgba, x, y);
                        if before != after {
                            changed += 1;
                            assert!(super::capture_border_pixel(x, y));
                            assert!(linkr_original_x_arm(x, y).is_none());
                            assert!(!super::center_online_pixel(x, y));
                            assert_eq!(before, super::READY_BACKGROUND);
                            assert_eq!(after, super::IDENTITY_GREEN);
                        }
                    }
                }
                assert!(changed > 0);
                assert_eq!(pixel(&capture_rgba, 15, 1), super::IDENTITY_GREEN);
                assert_eq!(idle.publication_key() ^ capture.publication_key(), 0x40);
            }
        }
    }

    #[test]
    fn starting_and_offline_keep_the_full_original_mark_and_static_status_color() {
        let endpoints = [(0_u8, 8, 8), (1, 22, 8), (2, 22, 22), (3, 8, 22)];
        let indicator = IndicatorState::default();

        for (state, background) in [
            (ServiceState::Starting, super::STARTING_BACKGROUND),
            (ServiceState::Offline, super::OFFLINE_BACKGROUND),
        ] {
            let rgba = status_rgba(IconFrame {
                state,
                indicator,
                heartbeat_on: true,
            });
            assert_eq!(
                rgba,
                status_rgba(IconFrame {
                    state,
                    indicator,
                    heartbeat_on: false
                })
            );
            for (_, x, y) in endpoints {
                assert_eq!(pixel(&rgba, x, y), super::WHITE);
            }
            assert_eq!(pixel(&rgba, 15, 1), background);
            assert_eq!(pixel(&rgba, 0, 0), super::TRANSPARENT);
        }
    }

    #[test]
    fn heartbeat_only_changes_center_pixels_including_during_capture() {
        for logic in [false, true] {
            let on = IconFrame {
                state: ServiceState::Ready,
                indicator: IndicatorState::new([true, false, false, true], logic),
                heartbeat_on: true,
            };
            let off = IconFrame {
                heartbeat_on: false,
                ..on
            };
            let on_rgba = status_rgba(on);
            let off_rgba = status_rgba(off);
            let mut changed = 0;
            for (index, (before, after)) in on_rgba
                .as_chunks::<4>()
                .0
                .iter()
                .zip(off_rgba.as_chunks::<4>().0)
                .enumerate()
            {
                if before != after {
                    changed += 1;
                    assert!(super::center_online_pixel(index % 32, index / 32));
                }
            }
            assert!(changed > 0);
            assert_eq!(pixel(&on_rgba, 15, 15), super::ONLINE_ACTIVE);
            assert_eq!(pixel(&off_rgba, 15, 15), super::ONLINE_IDLE);
            assert_eq!(on.publication_key() ^ off.publication_key(), 0x100);
        }
    }

    #[test]
    fn publication_keys_have_the_frozen_bit_layout() {
        let base = IconFrame {
            state: ServiceState::Ready,
            indicator: IndicatorState::new([false; 4], false),
            heartbeat_on: false,
        };
        assert_eq!(base.publication_key(), 0x82);
        assert_eq!(
            IconFrame {
                indicator: IndicatorState::new([true; 4], true),
                heartbeat_on: true,
                ..base
            }
            .publication_key(),
            0x1fe
        );
        for (indicator, expected) in [
            (
                IndicatorState::new([true, false, false, false], false),
                0x86,
            ),
            (
                IndicatorState::new([false, true, false, false], false),
                0x8a,
            ),
            (
                IndicatorState::new([false, false, false, true], false),
                0xa2,
            ),
            (
                IndicatorState::new([false, false, true, false], false),
                0x92,
            ),
            (IndicatorState::new([false; 4], true), 0xc2),
            (IndicatorState::new([true; 4], true), 0xfe),
            (IndicatorState::default(), 2),
        ] {
            assert_eq!(IconFrame { indicator, ..base }.publication_key(), expected);
        }
        assert_eq!(
            IconFrame {
                heartbeat_on: true,
                ..base
            }
            .publication_key(),
            0x182
        );
        assert_eq!(
            IconFrame {
                state: ServiceState::Starting,
                heartbeat_on: true,
                ..base
            }
            .publication_key(),
            0
        );
        assert_eq!(
            IconFrame {
                state: ServiceState::Offline,
                heartbeat_on: true,
                ..base
            }
            .publication_key(),
            1
        );
    }

    #[test]
    fn all_possible_frames_have_bounded_keys_with_one_rgba_per_key() {
        let mut by_key = std::collections::BTreeMap::<u16, (IconFrame, Vec<u8>)>::new();
        for state in [
            ServiceState::Starting,
            ServiceState::Offline,
            ServiceState::Ready,
        ] {
            for bits in 0_u16..128 {
                let frame = IconFrame {
                    state,
                    indicator: IndicatorState {
                        rails: [bits & 1 != 0, bits & 2 != 0, bits & 4 != 0, bits & 8 != 0],
                        logic_analyzer_active: bits & 16 != 0,
                        board_online: bits & 32 != 0,
                    },
                    heartbeat_on: bits & 64 != 0,
                };
                let key = frame.publication_key();
                let rgba = status_rgba(frame);
                assert!(key <= 0x1ff);
                assert_eq!(rgba, status_rgba(frame.effective()));
                if let Some((effective, previous)) = by_key.get(&key) {
                    assert_eq!(
                        &frame.effective(),
                        effective,
                        "no effective-state key collision"
                    );
                    assert_eq!(&rgba, previous, "publication key {key:#05x}");
                } else {
                    by_key.insert(key, (frame.effective(), rgba));
                }
            }
        }
        assert_eq!(by_key.len(), 98);
    }

    #[test]
    fn repeated_same_phase_samples_do_not_republish_for_any_service_or_activity_state() {
        let start = std::time::Instant::now();
        for state in [
            ServiceState::Starting,
            ServiceState::Ready,
            ServiceState::Offline,
        ] {
            for logic in [false, true] {
                let frame = IconFrame {
                    state,
                    indicator: IndicatorState::new([true, false, false, true], logic),
                    heartbeat_on: true,
                };
                let mut cache = super::IconCache::new(frame);
                let mut publications = 0;
                for sample in 1..=36_000 {
                    assert!(!cache
                        .publish_at(
                            frame,
                            start + std::time::Duration::from_millis(sample * 100),
                            |_, _| {
                                publications += 1;
                                Ok(())
                            },
                        )
                        .unwrap());
                }
                assert_eq!(publications, 0);
            }
        }
    }

    #[test]
    fn every_power_change_publishes_immediately_even_during_capture() {
        let start = std::time::Instant::now();
        for heartbeat_on in [false, true] {
            for bits in 0..16 {
                let initial = IconFrame {
                    state: ServiceState::Ready,
                    indicator: IndicatorState::new(rails(bits), false),
                    heartbeat_on,
                };
                let captured = IconFrame {
                    indicator: initial.indicator.with_host_activity(true),
                    ..initial
                };
                for arm in 0..4 {
                    let mut cache = super::IconCache::new(initial);
                    let mut publications = 0;
                    let mut changed = captured;
                    changed.indicator.rails[arm] = !changed.indicator.rails[arm];
                    let stopped = IconFrame {
                        indicator: changed.indicator.with_host_activity(false),
                        ..changed
                    };
                    // No elapsed time is needed for capture start, a real rail change,
                    // or capture stop. All three keep the current heartbeat phase.
                    for frame in [captured, changed, stopped] {
                        assert!(cache
                            .publish_at(frame, start, |key, rgba| {
                                publications += 1;
                                assert_eq!(key, frame.publication_key());
                                assert_eq!(rgba, status_rgba(frame));
                                Ok(())
                            })
                            .unwrap());
                        assert!(!cache
                            .publish_at(frame, start, |_, _| panic!("same state must not repeat"))
                            .unwrap());
                    }
                    assert_eq!(publications, 3);
                    assert_eq!(cache.rgba, status_rgba(stopped));
                }
            }
        }
    }

    #[test]
    fn failed_publication_keeps_the_previous_frame_and_retries_identical_input() {
        let now = std::time::Instant::now();
        let initial = IconFrame {
            state: ServiceState::Starting,
            indicator: IndicatorState::default(),
            heartbeat_on: false,
        };
        let changed = IconFrame {
            state: ServiceState::Ready,
            indicator: IndicatorState::new([false; 4], true),
            heartbeat_on: true,
        };
        let mut cache = super::IconCache::new(initial);
        let previous = cache.rgba.clone();
        let mut attempts = 0;
        assert!(cache
            .publish_at(changed, now, |_, _| {
                attempts += 1;
                anyhow::bail!("simulated icon publication failure")
            })
            .is_err());
        assert_eq!(cache.frame, initial);
        assert_eq!(cache.rgba, previous);
        for sample in 1..10 {
            // Pulse phase changes alone are the same failed underlying state.
            let repeated = IconFrame {
                heartbeat_on: sample % 2 == 0,
                ..changed
            };
            assert!(!cache
                .publish_at(
                    repeated,
                    now + std::time::Duration::from_millis(sample * 100),
                    |_, _| panic!("pulse must not bypass retry deadline"),
                )
                .unwrap());
        }
        assert_eq!(cache.frame, initial);
        assert_eq!(cache.rgba, previous);
        assert!(cache
            .publish_at(changed, now + super::ICON_RETRY_INTERVAL, |_, _| {
                attempts += 1;
                Ok(())
            })
            .unwrap());
        assert_eq!(attempts, 2);
        assert_eq!(cache.frame, changed);
        assert_ne!(cache.rgba, previous);
        assert_eq!(cache.failed, None);
        for seconds in 1..=60 {
            assert!(!cache
                .publish_at(
                    changed,
                    now + std::time::Duration::from_secs(seconds),
                    |_, _| panic!("already published"),
                )
                .unwrap());
        }
    }

    #[test]
    fn a_new_effective_state_bypasses_retry_delay_and_success_clears_failure() {
        let now = std::time::Instant::now();
        let initial = IconFrame {
            state: ServiceState::Starting,
            indicator: IndicatorState::default(),
            heartbeat_on: false,
        };
        let failed = IconFrame {
            state: ServiceState::Ready,
            indicator: IndicatorState::new([true, false, false, false], false),
            heartbeat_on: true,
        };
        let changed = IconFrame {
            indicator: IndicatorState::new([true, true, false, false], false),
            ..failed
        };
        let mut cache = super::IconCache::new(initial);
        assert!(cache
            .publish_at(failed, now, |_, _| anyhow::bail!("backend unavailable"))
            .is_err());
        assert!(cache
            .publish_at(
                changed,
                now + std::time::Duration::from_millis(100),
                |_, _| Ok(())
            )
            .unwrap());
        assert_eq!(cache.failed, None);
        // Returning to the formerly failed state is also an immediate transition.
        assert!(cache
            .publish_at(
                failed,
                now + std::time::Duration::from_millis(200),
                |_, _| Ok(())
            )
            .unwrap());
        assert_eq!(cache.failed, None);
        assert!(!cache
            .publish_at(
                failed,
                now + std::time::Duration::from_millis(300),
                |_, _| panic!("successful same-phase frame must be deduplicated")
            )
            .unwrap());
    }

    #[test]
    fn each_real_rail_change_during_capture_bypasses_failure_backoff() {
        let now = Instant::now();
        for arm in 0..4 {
            let initial = IconFrame {
                state: ServiceState::Starting,
                indicator: IndicatorState::default(),
                heartbeat_on: false,
            };
            let failed = IconFrame {
                state: ServiceState::Ready,
                indicator: IndicatorState::new([false; 4], true),
                heartbeat_on: true,
            };
            let mut changed = failed;
            changed.indicator.rails[arm] = true;
            let mut cache = super::IconCache::new(initial);
            assert!(cache
                .publish_at(failed, now, |_, _| anyhow::bail!("backend unavailable"))
                .is_err());
            assert!(cache
                .publish_at(changed, now + Duration::from_millis(100), |key, rgba| {
                    assert_eq!(key & 0x40, 0x40, "capture remains active");
                    assert_ne!(key, failed.publication_key());
                    assert_eq!(rgba, status_rgba(changed));
                    Ok(())
                })
                .unwrap());
            assert_eq!(cache.failed, None);
        }
    }

    #[test]
    fn failed_heartbeat_edges_share_backoff_even_when_phase_matches_last_success() {
        let start = std::time::Instant::now();
        let on = IconFrame {
            state: ServiceState::Ready,
            indicator: IndicatorState::new([true, false, false, true], false),
            heartbeat_on: true,
        };
        let off = IconFrame {
            heartbeat_on: false,
            ..on
        };
        let mut cache = super::IconCache::new(on);
        let previous = cache.rgba.clone();
        let mut attempts = 0;
        assert!(cache
            .publish_at(
                off,
                start + std::time::Duration::from_millis(200),
                |_, _| {
                    attempts += 1;
                    anyhow::bail!("backend unavailable")
                }
            )
            .is_err());
        for (millis, heartbeat_on) in [
            (300, true),
            (400, false),
            (800, true),
            (1_000, false),
            (1_100, true),
        ] {
            let frame = IconFrame { heartbeat_on, ..on };
            assert!(!cache
                .publish_at(
                    frame,
                    start + std::time::Duration::from_millis(millis),
                    |_, _| panic!("heartbeat must not bypass backoff")
                )
                .unwrap());
            assert_eq!(cache.frame, on);
            assert_eq!(cache.rgba, previous);
            assert!(cache.failed.is_some());
        }
        // At the retry deadline publish the current dark phase, not a missed bright edge.
        assert!(cache
            .publish_at(
                off,
                start + std::time::Duration::from_millis(1_200),
                |key, rgba| {
                    attempts += 1;
                    assert_eq!(key & 0x100, 0);
                    assert_eq!(pixel(rgba, 15, 15), super::ONLINE_IDLE);
                    Ok(())
                }
            )
            .unwrap());
        assert_eq!(attempts, 2);
        assert_eq!(cache.failed, None);
        assert_eq!(cache.frame, off);
        assert!(!cache
            .publish_at(
                off,
                start + std::time::Duration::from_millis(1_201),
                |_, _| panic!("same phase must not publish")
            )
            .unwrap());
        assert!(cache
            .publish_at(
                on,
                start + std::time::Duration::from_millis(1_600),
                |_, _| Ok(())
            )
            .unwrap());
    }
}
