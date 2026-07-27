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
const WCH_USB_VID: u16 = 0x1a86;
const CH347_USB_PID: u16 = 0x55de;

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
        Self::auto_detect_excluding(&[])
    }

    /// Try to auto-detect a CH347F serial port while excluding paths already assigned
    /// to another UART channel.
    pub fn auto_detect_excluding(excluded_paths: &[&str]) -> Result<Self> {
        let ports = serialport::available_ports().context("failed to list serial ports")?;
        let mut candidates = ports
            .iter()
            .filter(|port| is_ch347_serial_port(port))
            .filter(|port| !excluded_paths.contains(&port.port_name.as_str()))
            .collect::<Vec<_>>();
        candidates.sort_by(|left, right| left.port_name.cmp(&right.port_name));
        if let Some(port) = candidates.first() {
            return Self::open(&port.port_name, 115200);
        }
        bail!(
            "no CH347F serial port found. Available ports: {}",
            ports
                .iter()
                .map(|p| p.port_name.as_str())
                .collect::<Vec<_>>()
                .join(", ")
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

    /// Get unread bytes from cursor position.
    fn unread_bytes(&self) -> &[u8] {
        if self.cursor >= self.buffer.len() {
            return &[];
        }
        &self.buffer[self.cursor..]
    }

    /// Advance cursor by consumed bytes.
    fn consume(&mut self, len: usize) {
        self.cursor = (self.cursor + len).min(self.buffer.len());
    }

    /// Wait for a regex pattern in the serial output.
    pub fn wait_for_pattern(&mut self, pattern: &str, timeout: Duration) -> Result<PatternMatch> {
        let re = Regex::new(pattern).with_context(|| format!("invalid regex: {pattern}"))?;
        let deadline = Instant::now() + timeout;

        loop {
            self.fill_buffer()?;
            let stripped = strip_ansi_with_offsets(self.unread_bytes());

            if let Some(m) = re.find(&stripped.text) {
                self.consume(stripped.raw_offset(m.end()));
                return Ok(PatternMatch {
                    output: stripped.text,
                    timed_out: false,
                });
            }

            if Instant::now() >= deadline {
                return Ok(PatternMatch {
                    output: strip_ansi_with_offsets(self.unread_bytes()).text,
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
            let stripped = strip_ansi_with_offsets(self.unread_bytes());

            if let Some(m) = re.find(&stripped.text) {
                // Extract exit code from marker
                let marker_match = m.as_str();
                let exit_code: i32 = marker_match
                    .split(':')
                    .next_back()
                    .and_then(|s| s.parse().ok())
                    .unwrap_or(-1);

                // Output is everything before the marker
                let output_end = m.start();
                let output = stripped.text[..output_end].to_string();
                self.consume(stripped.raw_offset(m.end()));

                // Check pattern
                let pattern_re =
                    Regex::new(pattern).with_context(|| format!("invalid regex: {pattern}"))?;
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
                    output: strip_ansi_with_offsets(self.unread_bytes()).text,
                    pattern_matched: false,
                });
            }

            std::thread::sleep(Duration::from_millis(50));
        }
    }
}

fn is_ch347_serial_port(port: &serialport::SerialPortInfo) -> bool {
    match &port.port_type {
        serialport::SerialPortType::UsbPort(usb) => {
            usb.vid == WCH_USB_VID && usb.pid == CH347_USB_PID
        }
        _ => port.port_name.contains("ttyUSB") || port.port_name.contains("usbserial"),
    }
}

pub struct PatternMatch {
    pub output: String,
    pub timed_out: bool,
}

pub struct ExpectResult {
    pub completed: bool,
    pub exit_code: i32,
    pub output: String,
    pub pattern_matched: bool,
}

struct StrippedText {
    text: String,
    /// Clean UTF-8 byte boundary to source byte boundary.
    boundaries: Vec<(usize, usize)>,
}

impl StrippedText {
    fn raw_offset(&self, clean_offset: usize) -> usize {
        if clean_offset == 0 {
            return 0;
        }
        self.boundaries
            .iter()
            .find_map(|(clean, raw)| (*clean == clean_offset).then_some(*raw))
            .unwrap_or_else(|| self.boundaries.last().map(|(_, raw)| *raw).unwrap_or(0))
    }
}

fn strip_ansi_with_offsets(raw: &[u8]) -> StrippedText {
    let mut text = String::with_capacity(raw.len());
    let mut boundaries = Vec::with_capacity(raw.len());
    let mut index = 0;

    while index < raw.len() {
        if raw[index] == 0x1b {
            index += 1;
            if raw.get(index) == Some(&b'[') {
                index += 1;
                while index < raw.len() {
                    let byte = raw[index];
                    index += 1;
                    if (0x40..=0x7e).contains(&byte) {
                        break;
                    }
                }
            }
            continue;
        }
        if raw[index] == b'\r' {
            index += 1;
            continue;
        }

        let (character, byte_count) = match std::str::from_utf8(&raw[index..]) {
            Ok(valid) => {
                let character = valid.chars().next().expect("non-empty UTF-8 slice");
                (character, character.len_utf8())
            }
            Err(error) if error.valid_up_to() > 0 => {
                let valid = std::str::from_utf8(&raw[index..index + error.valid_up_to()])
                    .expect("validated UTF-8 prefix");
                let character = valid.chars().next().expect("non-empty UTF-8 prefix");
                (character, character.len_utf8())
            }
            Err(error) => ('\u{fffd}', error.error_len().unwrap_or(1)),
        };
        index += byte_count;
        text.push(character);
        boundaries.push((text.len(), index));
    }

    StrippedText { text, boundaries }
}

#[cfg(test)]
fn strip_ansi(s: &str) -> String {
    strip_ansi_with_offsets(s.as_bytes()).text
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strips_ansi_and_maps_match_end_to_raw_bytes() {
        let raw = b"\x1b[31mlogin:\x1b[0mnext";
        let stripped = strip_ansi_with_offsets(raw);
        assert_eq!(stripped.text, "login:next");
        assert_eq!(stripped.raw_offset("login:".len()), b"\x1b[31mlogin:".len());
    }

    #[test]
    fn maps_utf8_boundaries_without_using_character_counts() {
        let raw = "\x1b[32m中文 login:\r\n".as_bytes();
        let stripped = strip_ansi_with_offsets(raw);
        assert_eq!(stripped.text, "中文 login:\n");
        let match_end = stripped.text.find("login:").unwrap() + "login:".len();
        assert_eq!(stripped.raw_offset(match_end), "\x1b[32m中文 login:".len());
    }

    #[test]
    fn compatibility_strip_ansi_removes_carriage_returns() {
        assert_eq!(strip_ansi("\x1b[31mhello\x1b[0m\r\n"), "hello\n");
    }

    #[test]
    fn detects_ch347_usbmodem_ports_by_vid_and_pid() {
        let port = serialport::SerialPortInfo {
            port_name: "/dev/cu.usbmodemBD5ACDABCD1".to_string(),
            port_type: serialport::SerialPortType::UsbPort(serialport::UsbPortInfo {
                vid: WCH_USB_VID,
                pid: CH347_USB_PID,
                serial_number: Some("BD5ACDABCD".to_string()),
                manufacturer: Some("wch.cn".to_string()),
                product: Some("UART_SPI_I2C_JTAG".to_string()),
            }),
        };
        assert!(is_ch347_serial_port(&port));
    }
}
