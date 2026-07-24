// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Serial communication for test automation.
// Handles CH347F UART channels and the linkr task marker protocol.

use anyhow::{bail, Context, Result};
use regex::Regex;
use std::io::{Read, Write};
use std::time::{Duration, Instant};

const BUFFER_CAPACITY: usize = 64 * 1024;
const MARKER_PREFIX: &str = "__LINKR_TASK_";

pub struct SerialPort {
    port: Box<dyn serialport::SerialPort>,
    buffer: Vec<u8>,
    cursor: usize,
}

impl SerialPort {
    pub fn open(path: &str, baud_rate: u32) -> Result<Self> {
        let port = serialport::new(path, baud_rate)
            .timeout(Duration::from_millis(100))
            .open()
            .with_context(|| format!("failed to open serial port {path:?}"))?;
        Ok(Self {
            port,
            buffer: Vec::with_capacity(BUFFER_CAPACITY),
            cursor: 0,
        })
    }

    /// Try to auto-detect a CH347F serial port.
    pub fn auto_detect() -> Result<Self> {
        let ports = serialport::available_ports()
            .context("failed to list serial ports")?;
        for port_info in &ports {
            let name = &port_info.port_name;
            // CH347F typically appears as /dev/ttyUSB* on Linux,
            // /dev/tty.usbserial-* on macOS, or COM* on Windows
            if name.contains("ttyUSB") || name.contains("usbserial") {
                return Self::open(name, 115200);
            }
        }
        bail!(
            "no CH347F serial port found. Available ports: {}",
            ports.iter().map(|p| p.port_name.as_str()).collect::<Vec<_>>().join(", ")
        );
    }

    /// Write text to the serial port.
    pub fn write(&mut self, text: &str) -> Result<()> {
        self.port.write_all(text.as_bytes())?;
        self.port.flush()?;
        Ok(())
    }

    /// Read available bytes into the circular buffer.
    fn fill_buffer(&mut self) -> Result<()> {
        let mut tmp = [0u8; 4096];
        loop {
            match self.port.read(&mut tmp) {
                Ok(0) => break,
                Ok(n) => {
                    // Trim buffer if near capacity
                    if self.buffer.len() + n > BUFFER_CAPACITY {
                        let drain = self.buffer.len() + n - BUFFER_CAPACITY;
                        if drain > self.cursor {
                            self.cursor = 0;
                        } else {
                            self.cursor -= drain;
                        }
                        self.buffer.drain(..drain);
                    }
                    self.buffer.extend_from_slice(&tmp[..n]);
                }
                Err(e) if e.kind() == std::io::ErrorKind::TimedOut => break,
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                Err(e) => return Err(e.into()),
            }
        }
        Ok(())
    }

    /// Get unread text from cursor position.
    fn unread_text(&self) -> String {
        if self.cursor >= self.buffer.len() {
            return String::new();
        }
        String::from_utf8_lossy(&self.buffer[self.cursor..]).to_string()
    }

    /// Advance cursor by consumed bytes.
    fn consume(&mut self, len: usize) {
        self.cursor = (self.cursor + len).min(self.buffer.len());
    }

    /// Wait for a regex pattern in the serial output.
    pub fn wait_for_pattern(
        &mut self,
        pattern: &str,
        timeout: Duration,
    ) -> Result<PatternMatch> {
        let re = Regex::new(pattern).with_context(|| format!("invalid regex: {pattern}"))?;
        let deadline = Instant::now() + timeout;

        loop {
            self.fill_buffer()?;
            let text = self.unread_text();
            let stripped = strip_ansi(&text);

            if let Some(m) = re.find(&stripped) {
                let end = m.end();
                self.consume(end);
                return Ok(PatternMatch {
                    matched: true,
                    output: stripped,
                    timed_out: false,
                });
            }

            if Instant::now() >= deadline {
                return Ok(PatternMatch {
                    matched: false,
                    output: strip_ansi(&self.unread_text()),
                    timed_out: true,
                });
            }

            std::thread::sleep(Duration::from_millis(50));
        }
    }

    /// Send a command and wait for the linkr task marker with exit code.
    pub fn send_and_expect(
        &mut self,
        command: &str,
        pattern: &str,
        timeout: Duration,
    ) -> Result<ExpectResult> {
        let run_id = uuid::Uuid::new_v4().to_string().replace('-', "")[..8].to_string();
        let marker = format!("{MARKER_PREFIX}{run_id}__");

        // Send command wrapped in envelope
        let envelope = format!(
            "{command}\r\n__linkr_task_status=$?\r\nprintf '\\n{marker}:%s\\n' \"$__linkr_task_status\"\r\n"
        );
        self.write(&envelope)?;

        // Wait for marker
        let marker_pattern = format!("{}:\\d+", regex::escape(&marker));
        let deadline = Instant::now() + timeout;
        let re = Regex::new(&marker_pattern)?;

        loop {
            self.fill_buffer()?;
            let text = self.unread_text();
            let stripped = strip_ansi(&text);

            if let Some(m) = re.find(&stripped) {
                // Extract exit code from marker
                let marker_match = m.as_str();
                let exit_code: i32 = marker_match
                    .split(':')
                    .last()
                    .and_then(|s| s.parse().ok())
                    .unwrap_or(-1);

                // Output is everything before the marker
                let output_end = m.start();
                let output = stripped[..output_end].to_string();
                self.consume(m.end());

                // Check pattern
                let pattern_re = Regex::new(pattern)
                    .with_context(|| format!("invalid regex: {pattern}"))?;
                let pattern_matched = pattern_re.is_match(&output);

                return Ok(ExpectResult {
                    completed: true,
                    exit_code,
                    output,
                    pattern_matched,
                });
            }

            if Instant::now() >= deadline {
                return Ok(ExpectResult {
                    completed: false,
                    exit_code: -1,
                    output: strip_ansi(&self.unread_text()),
                    pattern_matched: false,
                });
            }

            std::thread::sleep(Duration::from_millis(50));
        }
    }
}

pub struct PatternMatch {
    pub matched: bool,
    pub output: String,
    pub timed_out: bool,
}

pub struct ExpectResult {
    pub completed: bool,
    pub exit_code: i32,
    pub output: String,
    pub pattern_matched: bool,
}

fn strip_ansi(s: &str) -> String {
    let mut result = String::with_capacity(s.len());
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\x1b' {
            if chars.peek() == Some(&'[') {
                chars.next();
                while let Some(&next) = chars.peek() {
                    if next.is_ascii_alphabetic() || next == 'm' {
                        chars.next();
                        break;
                    }
                    chars.next();
                }
            }
        } else if c != '\r' {
            // Also strip carriage returns
            result.push(c);
        }
    }
    result
}
