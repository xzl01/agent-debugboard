use glib::SourceId;
use std::{
    cell::RefCell,
    rc::Rc,
    time::{Duration, Instant},
};
use tao::event_loop::EventLoopProxy;

use super::UserEvent;

// Tao's Linux WaitUntil does not arm a GTK timer. Wake its main context at the
// next heartbeat edge, without polling or relying on probes/desktop input.
pub(super) struct LinuxHeartbeatWake {
    source: Rc<RefCell<Option<SourceId>>>,
    wake: Rc<dyn Fn()>,
    last_deadline: Option<Instant>,
}

impl LinuxHeartbeatWake {
    pub(super) fn new(proxy: EventLoopProxy<UserEvent>) -> Self {
        Self::with_wake(move || {
            let _ = proxy.send_event(UserEvent::HeartbeatWake);
        })
    }

    fn with_wake(wake: impl Fn() + 'static) -> Self {
        Self {
            source: Rc::new(RefCell::new(None)),
            wake: Rc::new(wake),
            last_deadline: None,
        }
    }

    pub(super) fn schedule(&mut self, next: Option<Instant>) {
        if next == self.last_deadline {
            return;
        }
        self.cancel_source();
        self.last_deadline = next;
        let Some(deadline) = next else {
            return;
        };
        let source = Rc::clone(&self.source);
        let wake = Rc::clone(&self.wake);
        let id = glib::timeout_add_local_once(timer_delay(Instant::now(), deadline), move || {
            // GLib removes this one-shot itself. Forget its ID before sending the
            // event so cancellation/Drop cannot remove an already-completed source.
            source.borrow_mut().take();
            wake();
        });
        *self.source.borrow_mut() = Some(id);
    }

    fn cancel_source(&mut self) {
        let source = self.source.borrow_mut().take();
        if let Some(source) = source {
            source.remove();
        }
    }
}

impl Drop for LinuxHeartbeatWake {
    fn drop(&mut self) {
        self.cancel_source();
    }
}

fn timer_delay(now: Instant, deadline: Instant) -> Duration {
    // GLib 0.18 truncates durations to milliseconds. Round up ourselves to avoid
    // early edge events, and use at least 1 ms even for an already-due deadline.
    let millis = deadline
        .saturating_duration_since(now)
        .as_nanos()
        .div_ceil(1_000_000)
        .max(1);
    Duration::from_millis(u64::try_from(millis).expect("heartbeat timer delay fits milliseconds"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    #[test]
    fn delay_rounds_up_to_whole_milliseconds_and_is_never_zero() {
        let now = Instant::now();
        for (nanos, millis) in [
            (0, 1),
            (1, 1),
            (999_999, 1),
            (1_000_000, 1),
            (1_000_001, 2),
            (1_999_999, 2),
            (2_000_000, 2),
            (2_000_001, 3),
        ] {
            assert_eq!(
                timer_delay(now, now + Duration::from_nanos(nanos)),
                Duration::from_millis(millis)
            );
        }
        assert_eq!(
            timer_delay(now + Duration::from_millis(1), now),
            Duration::from_millis(1)
        );
    }

    // Keep default-context lifecycle checks in one test: no parallel acquisition
    // of GLib's global context, and no GTK/display/Tao event loop is initialized.
    #[test]
    fn single_shot_wakes_without_probes_and_deduplicates_cancels_and_drops_cleanly() {
        let context = glib::MainContext::default();
        let _ownership = context.acquire().expect("own default GLib context");
        let main_loop = glib::MainLoop::new(Some(&context), false);
        let wakes = Rc::new(Cell::new(0));
        let mut timer = LinuxHeartbeatWake::with_wake({
            let wakes = Rc::clone(&wakes);
            let main_loop = main_loop.clone();
            move || {
                wakes.set(wakes.get() + 1);
                main_loop.quit();
            }
        });

        let deadline = Instant::now() + Duration::from_secs(60);
        timer.schedule(Some(deadline));
        let first = timer.source.borrow().as_ref().unwrap().as_raw();
        timer.schedule(Some(deadline));
        assert_eq!(timer.source.borrow().as_ref().unwrap().as_raw(), first);
        timer.schedule(Some(deadline + Duration::from_secs(1)));
        assert_ne!(timer.source.borrow().as_ref().unwrap().as_raw(), first);
        timer.schedule(None); // Offline/unknown cancels the pending edge.
        timer.schedule(None);
        assert!(timer.source.borrow().is_none());
        assert_eq!(timer.last_deadline, None);

        let mut dropped = LinuxHeartbeatWake::with_wake(|| panic!("dropped timer fired"));
        dropped.schedule(Some(Instant::now()));
        let dropped_source = Rc::downgrade(&dropped.source);
        drop(dropped);
        assert!(
            dropped_source.upgrade().is_none(),
            "no callback ownership cycle"
        );

        // This is a real GLib single-shot waking a blocked main loop, not a clock
        // simulation. No probe or desktop event drives the injected callback.
        timer.schedule(Some(Instant::now() + Duration::from_millis(1)));
        let watchdog_fired = Rc::new(Cell::new(false));
        let watchdog = glib::timeout_add_local_once(Duration::from_secs(2), {
            let watchdog_fired = Rc::clone(&watchdog_fired);
            let main_loop = main_loop.clone();
            move || {
                watchdog_fired.set(true);
                main_loop.quit();
            }
        });
        main_loop.run();
        if !watchdog_fired.get() {
            watchdog.remove();
        }
        assert_eq!(wakes.get(), 1);
        assert!(timer.source.borrow().is_none());
        timer.schedule(None); // Must not remove the already-completed source.
        timer.schedule(Some(deadline));
        let source = Rc::downgrade(&timer.source);
        drop(timer);
        assert!(source.upgrade().is_none());
    }
}
