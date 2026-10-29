//! Never call the serial driver's synchronous tcdrain/FlushFileBuffers from poll.
use std::{
    future::Future,
    io,
    pin::Pin,
    task::{Context, Poll},
    time::Duration,
};

use tokio::io::{AsyncRead, AsyncWrite, ReadBuf};
use tokio_serial::{SerialPort, SerialStream};

pub(crate) trait OutputQueue {
    fn bytes_pending(&self) -> io::Result<u32>;
}

impl OutputQueue for SerialStream {
    fn bytes_pending(&self) -> io::Result<u32> {
        // serialport 4.9: TIOCOUTQ on POSIX; ClearCommError/COMSTAT on Windows.
        // This queries a queue, not wire-complete or peripheral FIFO state.
        self.bytes_to_write().map_err(io::Error::from)
    }
}

pub(crate) struct SerialIo<T> {
    inner: T,
    drain_poll: Option<Pin<Box<tokio::time::Sleep>>>,
}

impl<T> SerialIo<T> {
    pub(crate) fn new(inner: T) -> Self {
        Self {
            inner,
            drain_poll: None,
        }
    }
}

impl<T: AsyncRead + Unpin> AsyncRead for SerialIo<T> {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        Pin::new(&mut self.inner).poll_read(cx, buf)
    }
}

impl<T: AsyncWrite + OutputQueue + Unpin> AsyncWrite for SerialIo<T> {
    fn poll_write(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bytes: &[u8],
    ) -> Poll<io::Result<usize>> {
        self.drain_poll = None;
        Pin::new(&mut self.inner).poll_write(cx, bytes)
    }

    fn poll_flush(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        if let Some(timer) = self.drain_poll.as_mut() {
            if timer.as_mut().poll(cx).is_pending() {
                return Poll::Pending;
            }
        }
        self.drain_poll = None;
        match self.inner.bytes_pending() {
            Ok(0) => Poll::Ready(Ok(())),
            Ok(_) => {
                let mut timer = Box::pin(tokio::time::sleep(Duration::from_millis(5)));
                let _ = timer.as_mut().poll(cx);
                self.drain_poll = Some(timer);
                Poll::Pending
            }
            Err(error) => Poll::Ready(Err(error)),
        }
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        // The underlying poll_shutdown can also call tcdrain.
        self.poll_flush(cx)
    }
}
