use super::terminal::{finish, TerminalOps, TerminalSession};
use anyhow::anyhow;
use std::cell::RefCell;
use std::io;
use std::rc::Rc;

#[derive(Clone, Default)]
struct SharedLog(Rc<RefCell<Vec<&'static str>>>);

impl SharedLog {
    fn entries(&self) -> Vec<&'static str> {
        self.0.borrow().clone()
    }
}

struct FakeOps {
    log: SharedLog,
    fail_on: Vec<&'static str>,
}

impl FakeOps {
    fn step(&mut self, name: &'static str) -> io::Result<()> {
        self.log.0.borrow_mut().push(name);
        if self.fail_on.contains(&name) {
            return Err(io::Error::other(name));
        }
        Ok(())
    }
}

impl TerminalOps for FakeOps {
    fn enable_raw_mode(&mut self) -> io::Result<()> {
        self.step("enable_raw")
    }
    fn disable_raw_mode(&mut self) -> io::Result<()> {
        self.step("disable_raw")
    }
    fn enter_alternate_screen(&mut self) -> io::Result<()> {
        self.step("enter_alt")
    }
    fn leave_alternate_screen(&mut self) -> io::Result<()> {
        self.step("leave_alt")
    }
    fn enable_mouse_capture(&mut self) -> io::Result<()> {
        self.step("enable_mouse")
    }
    fn disable_mouse_capture(&mut self) -> io::Result<()> {
        self.step("disable_mouse")
    }
    fn show_cursor(&mut self) -> io::Result<()> {
        self.step("show_cursor")
    }
}

fn enter_with(
    log: &SharedLog,
    fail_on: &[&'static str],
) -> anyhow::Result<TerminalSession<FakeOps>> {
    TerminalSession::enter_with(FakeOps {
        log: log.clone(),
        fail_on: fail_on.to_vec(),
    })
}

#[test]
fn enter_rolls_back_raw_mode_when_alternate_screen_fails() {
    let log = SharedLog::default();
    let result = enter_with(&log, &["enter_alt"]);

    assert!(result.is_err());
    assert_eq!(log.entries(), ["enable_raw", "enter_alt", "disable_raw"]);
}

#[test]
fn enter_rolls_back_alt_screen_and_raw_mode_when_mouse_capture_fails() {
    let log = SharedLog::default();
    let result = enter_with(&log, &["enable_mouse"]);

    assert!(result.is_err());
    assert_eq!(
        log.entries(),
        [
            "enable_raw",
            "enter_alt",
            "enable_mouse",
            "leave_alt",
            "disable_raw"
        ]
    );
}

#[test]
fn leave_attempts_every_cleanup_step_and_surfaces_the_first_error() {
    let log = SharedLog::default();
    let mut session = enter_with(&log, &[]).unwrap();
    session.ops_mut().fail_on.push("disable_mouse");

    let cleanup = session.leave();

    assert_eq!(cleanup.unwrap_err().to_string(), "disable_mouse");
    assert_eq!(
        log.entries(),
        [
            "enable_raw",
            "enter_alt",
            "enable_mouse",
            "disable_mouse",
            "leave_alt",
            "show_cursor",
            "disable_raw",
        ]
    );
}

#[test]
fn leave_is_idempotent() {
    let log = SharedLog::default();
    let mut session = enter_with(&log, &[]).unwrap();

    session.leave().unwrap();
    let entries_after_first = log.entries().len();
    session.leave().unwrap();

    assert_eq!(log.entries().len(), entries_after_first);
}

#[test]
fn finish_prefers_the_event_loop_error_over_cleanup_errors() {
    let log = SharedLog::default();
    let mut session = enter_with(&log, &[]).unwrap();
    session.ops_mut().fail_on.push("disable_raw");

    let result = finish(Err(anyhow!("event loop broke")), &mut session);

    assert_eq!(result.unwrap_err().to_string(), "event loop broke");
}

#[test]
fn finish_surfaces_cleanup_error_when_the_loop_succeeded() {
    let log = SharedLog::default();
    let mut session = enter_with(&log, &[]).unwrap();
    session.ops_mut().fail_on.push("leave_alt");

    let result = finish(Ok(0), &mut session);

    assert_eq!(result.unwrap_err().to_string(), "leave_alt");
}

#[test]
fn finish_returns_the_exit_code_when_loop_and_cleanup_succeed() {
    let log = SharedLog::default();
    let mut session = enter_with(&log, &[]).unwrap();

    let result = finish(Ok(3), &mut session);

    assert_eq!(result.unwrap(), 3);
}
