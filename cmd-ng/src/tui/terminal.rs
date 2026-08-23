use anyhow::Result;
use crossterm::cursor::Show;
use crossterm::event::{DisableMouseCapture, EnableMouseCapture};
use crossterm::execute;
use crossterm::terminal::{
    disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen,
};
use std::io::{self, Stdout};

pub(super) trait TerminalOps {
    fn enable_raw_mode(&mut self) -> io::Result<()>;
    fn disable_raw_mode(&mut self) -> io::Result<()>;
    fn enter_alternate_screen(&mut self) -> io::Result<()>;
    fn leave_alternate_screen(&mut self) -> io::Result<()>;
    fn enable_mouse_capture(&mut self) -> io::Result<()>;
    fn disable_mouse_capture(&mut self) -> io::Result<()>;
    fn show_cursor(&mut self) -> io::Result<()>;
}

pub(super) struct CrosstermOps {
    stdout: Stdout,
}

impl TerminalOps for CrosstermOps {
    fn enable_raw_mode(&mut self) -> io::Result<()> {
        enable_raw_mode()
    }
    fn disable_raw_mode(&mut self) -> io::Result<()> {
        disable_raw_mode()
    }
    fn enter_alternate_screen(&mut self) -> io::Result<()> {
        execute!(self.stdout, EnterAlternateScreen)
    }
    fn leave_alternate_screen(&mut self) -> io::Result<()> {
        execute!(self.stdout, LeaveAlternateScreen)
    }
    fn enable_mouse_capture(&mut self) -> io::Result<()> {
        execute!(self.stdout, EnableMouseCapture)
    }
    fn disable_mouse_capture(&mut self) -> io::Result<()> {
        execute!(self.stdout, DisableMouseCapture)
    }
    fn show_cursor(&mut self) -> io::Result<()> {
        execute!(self.stdout, Show)
    }
}

pub(super) struct TerminalSession<T: TerminalOps> {
    ops: T,
    active: bool,
}

impl TerminalSession<CrosstermOps> {
    pub(super) fn enter() -> Result<Self> {
        Self::enter_with(CrosstermOps {
            stdout: io::stdout(),
        })
    }
}

impl<T: TerminalOps> TerminalSession<T> {
    pub(super) fn enter_with(mut ops: T) -> Result<Self> {
        ops.enable_raw_mode()?;
        if let Err(err) = ops.enter_alternate_screen() {
            let _ = ops.disable_raw_mode();
            return Err(err.into());
        }
        if let Err(err) = ops.enable_mouse_capture() {
            let _ = ops.leave_alternate_screen();
            let _ = ops.disable_raw_mode();
            return Err(err.into());
        }
        Ok(Self { ops, active: true })
    }

    pub(super) fn leave(&mut self) -> io::Result<()> {
        if !self.active {
            return Ok(());
        }
        self.active = false;
        let mut first_error: Option<io::Error> = None;
        for result in [
            self.ops.disable_mouse_capture(),
            self.ops.leave_alternate_screen(),
            self.ops.show_cursor(),
            self.ops.disable_raw_mode(),
        ] {
            if let Err(err) = result {
                first_error.get_or_insert(err);
            }
        }
        match first_error {
            Some(err) => Err(err),
            None => Ok(()),
        }
    }

    #[cfg(test)]
    pub(super) fn ops_mut(&mut self) -> &mut T {
        &mut self.ops
    }
}

impl<T: TerminalOps> Drop for TerminalSession<T> {
    fn drop(&mut self) {
        let _ = self.leave();
    }
}

pub(super) fn finish<T: TerminalOps>(
    loop_result: Result<u8>,
    session: &mut TerminalSession<T>,
) -> Result<u8> {
    match loop_result {
        Err(err) => {
            let _ = session.leave();
            Err(err)
        }
        Ok(code) => {
            session.leave()?;
            Ok(code)
        }
    }
}
