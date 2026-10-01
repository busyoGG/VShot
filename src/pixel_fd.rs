// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Handing a pixel buffer to the Qt helper, and taking the rendered result back,
//! over a socket instead of through a file.
//!
//! Both directions carry the same thing: a short fixed header naming the buffer's
//! size and format, and a file descriptor holding the pixels themselves.  The
//! bytes travel as a `memfd` passed with `SCM_RIGHTS`, so the socket carries only
//! the header and neither side writes a temporary file — the pixels are a shared
//! mapping, and the reader maps them rather than copying them.
//!
//! The helper is started with one end of the pair already installed on a known
//! descriptor ([`CHILD_FD`]), which is the whole handshake: no path, no
//! negotiation, and nothing to clean up when either side dies.

use std::io;
use std::mem::size_of;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::os::unix::process::CommandExt;
use std::process::Command;

use crate::error::{Result, VshotError};

/// The descriptor the helper is handed, and the one it must use to answer.
///
/// A number rather than an inherited path, because the pair is made before the
/// child exists and there is nowhere to put a name.  `dup2` clears `FD_CLOEXEC`
/// on the copy, so this is the one descriptor that survives the exec.
pub(crate) const CHILD_FD: RawFd = 3;

/// The variable the helper reads the descriptor out of.  It is both the flag
/// that a channel exists and the number to use: a helper started by hand has no
/// channel, and neither does one whose fd 3 happens to be an unrelated
/// descriptor a shell left open.
pub(crate) const CHANNEL_ENV: &str = "VSHOT_PIXEL_FD";

/// The header's magic, so a message that is not one of ours is refused instead
/// of read as a size.
const MAGIC: &[u8; 4] = b"VSPX";
/// The layout version.  A helper that does not know it refuses the message
/// rather than reading a buffer it has misparsed.
const VERSION: u32 = 1;
/// Magic, version, kind, format, width, height, and four reserved bytes.
const HEADER: usize = 32;

/// What the buffer holds, so a result is never mistaken for a source image.
pub(crate) const KIND_SOURCE: u32 = 0;
pub(crate) const KIND_RESULT: u32 = 1;

/// Eight-bit RGBA, straight (non-premultiplied) alpha, one byte per channel.
/// The only format either side sends: the source image is already this, and the
/// helper converts its result to it before answering.
pub(crate) const FORMAT_RGBA8888: u32 = 1;

/// One end of the pair.
pub(crate) struct PixelChannel {
    fd: OwnedFd,
}

/// A buffer that arrived: its size, its format, and the pixels themselves as a
/// mapping of the sender's `memfd`.
#[derive(Debug)]
pub(crate) struct Received {
    pub map: memmap2::Mmap,
    pub kind: u32,
    pub format: u32,
    pub width: u32,
    pub height: u32,
}

impl Received {
    pub fn pixels(&self) -> &[u8] {
        &self.map
    }
}

fn failed(message: impl Into<String>) -> VshotError {
    VshotError::Selection(message.into())
}

impl PixelChannel {
    /// A connected pair: one end stays here, the other is installed in the
    /// helper.
    fn pair() -> Result<(Self, Self)> {
        let mut fds = [0 as RawFd; 2];
        // `SOCK_SEQPACKET` keeps one send to one receive, so the header is never
        // split across reads and the control message always arrives with it.
        let type_ = libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC;
        // SAFETY: `fds` is a two-element array, which is what the call writes.
        let rc = unsafe { libc::socketpair(libc::AF_UNIX, type_, 0, fds.as_mut_ptr()) };
        if rc != 0 {
            return Err(failed(format!(
                "failed to make the pixel channel: {}",
                io::Error::last_os_error()
            )));
        }
        // SAFETY: both descriptors are freshly created and owned by nobody else.
        let first = unsafe { OwnedFd::from_raw_fd(fds[0]) };
        // SAFETY: as above.
        let second = unsafe { OwnedFd::from_raw_fd(fds[1]) };
        Ok((Self { fd: first }, Self { fd: second }))
    }

    /// A pair for a helper that is about to be spawned: the end this side
    /// keeps, and the end the helper is handed.
    ///
    /// Both are returned because the child's end has to stay open until the
    /// child exists — the `pre_exec` hook dups it, and the copy is what the
    /// helper keeps — and then has to be closed here.  Holding it open past the
    /// spawn would leave a writer alive in this process, and the read of the
    /// helper's answer would wait on it.
    pub(crate) fn spawn_pair() -> Result<(Self, Self)> {
        Self::pair()
    }

    /// Hands `pixels` over as a `memfd`.
    pub(crate) fn send(
        &self,
        kind: u32,
        format: u32,
        width: u32,
        height: u32,
        pixels: &[u8],
    ) -> Result<()> {
        let expected = (width as usize)
            .checked_mul(height as usize)
            .and_then(|pixels| pixels.checked_mul(4))
            .ok_or_else(|| failed("pixel buffer dimensions overflow"))?;
        if pixels.len() != expected {
            return Err(failed(format!(
                "pixel buffer is {} bytes, expected {expected} for {width}x{height}",
                pixels.len()
            )));
        }
        let buffer = Memfd::create(pixels.len())?;
        buffer.write_all(pixels)?;

        let mut header = [0u8; HEADER];
        header[0..4].copy_from_slice(MAGIC);
        header[4..8].copy_from_slice(&VERSION.to_ne_bytes());
        header[8..12].copy_from_slice(&kind.to_ne_bytes());
        header[12..16].copy_from_slice(&format.to_ne_bytes());
        header[16..20].copy_from_slice(&width.to_ne_bytes());
        header[20..24].copy_from_slice(&height.to_ne_bytes());

        let mut iov = [libc::iovec {
            iov_base: header.as_mut_ptr().cast(),
            iov_len: HEADER,
        }];
        // SAFETY: `CMSG_SPACE` is a pure size computation for one descriptor.
        let control_len = unsafe { libc::CMSG_SPACE(size_of::<RawFd>() as u32) } as usize;
        let mut control = vec![0u8; control_len];
        // SAFETY: `msghdr` is a plain C struct; all-zero is a valid starting
        // point and every field that matters is set below.
        let mut message: libc::msghdr = unsafe { std::mem::zeroed() };
        message.msg_iov = iov.as_mut_ptr();
        message.msg_iovlen = 1;
        message.msg_control = control.as_mut_ptr().cast();
        message.msg_controllen = control_len;
        // SAFETY: the control buffer is `CMSG_SPACE` bytes, which is what the
        // first header needs, and the descriptor is valid.
        unsafe {
            let header = libc::CMSG_FIRSTHDR(&message);
            if header.is_null() {
                return Err(failed("failed to build the pixel channel message"));
            }
            (*header).cmsg_level = libc::SOL_SOCKET;
            (*header).cmsg_type = libc::SCM_RIGHTS;
            (*header).cmsg_len = libc::CMSG_LEN(size_of::<RawFd>() as u32) as usize;
            std::ptr::write_unaligned(libc::CMSG_DATA(header).cast::<RawFd>(), buffer.raw());
        }
        // SAFETY: the message is fully initialised above and the socket is ours.
        let sent = unsafe { libc::sendmsg(self.fd.as_raw_fd(), &message, 0) };
        if sent < 0 {
            return Err(failed(format!(
                "failed to send the pixel buffer: {}",
                io::Error::last_os_error()
            )));
        }
        Ok(())
    }

    /// Waits for one buffer and maps it.
    pub(crate) fn receive(&self) -> Result<Received> {
        let mut header = [0u8; HEADER];
        let mut iov = [libc::iovec {
            iov_base: header.as_mut_ptr().cast(),
            iov_len: HEADER,
        }];
        // SAFETY: as in `send`, a size computation for one descriptor.
        let control_len = unsafe { libc::CMSG_SPACE(size_of::<RawFd>() as u32) } as usize;
        let mut control = vec![0u8; control_len];
        // SAFETY: as in `send`.
        let mut message: libc::msghdr = unsafe { std::mem::zeroed() };
        message.msg_iov = iov.as_mut_ptr();
        message.msg_iovlen = 1;
        message.msg_control = control.as_mut_ptr().cast();
        message.msg_controllen = control_len;
        // SAFETY: the message points at live buffers of the stated lengths.
        let read = unsafe { libc::recvmsg(self.fd.as_raw_fd(), &mut message, 0) };
        if read < 0 {
            return Err(failed(format!(
                "failed to read the pixel buffer: {}",
                io::Error::last_os_error()
            )));
        }
        if read == 0 {
            return Err(failed("the pixel channel closed before a buffer arrived"));
        }
        if read as usize != HEADER {
            return Err(failed(format!(
                "the pixel channel sent {read} bytes where a {HEADER}-byte header was due"
            )));
        }
        if &header[0..4] != MAGIC {
            return Err(failed(
                "the pixel channel sent a buffer that is not VShot's",
            ));
        }
        let number = |at: usize| u32::from_ne_bytes(header[at..at + 4].try_into().unwrap());
        if number(4) != VERSION {
            return Err(failed(format!(
                "the pixel channel speaks version {} where {VERSION} was expected",
                number(4)
            )));
        }
        let kind = number(8);
        let format = number(12);
        let width = number(16);
        let height = number(20);

        // SAFETY: the control buffer was filled by `recvmsg`, and a null first
        // header means no descriptor came with the message.
        let descriptor = unsafe {
            let header = libc::CMSG_FIRSTHDR(&message);
            if header.is_null()
                || (*header).cmsg_level != libc::SOL_SOCKET
                || (*header).cmsg_type != libc::SCM_RIGHTS
            {
                return Err(failed("the pixel channel sent no descriptor"));
            }
            std::ptr::read_unaligned(libc::CMSG_DATA(header).cast::<RawFd>())
        };
        // SAFETY: the descriptor came from `SCM_RIGHTS` and nothing else owns it.
        let descriptor = unsafe { OwnedFd::from_raw_fd(descriptor) };

        let expected = (width as usize)
            .checked_mul(height as usize)
            .and_then(|pixels| pixels.checked_mul(4))
            .ok_or_else(|| failed("pixel buffer dimensions overflow"))?;
        // SAFETY: the descriptor is an open, readable `memfd` whose size this
        // side chose; a buffer that is not is refused by the length check below.
        let map = unsafe { memmap2::Mmap::map(&descriptor) }
            .map_err(|error| failed(format!("failed to map the pixel buffer: {error}")))?;
        if map.len() != expected {
            return Err(failed(format!(
                "the pixel channel sent {} bytes, expected {expected} for {width}x{height}",
                map.len()
            )));
        }
        Ok(Received {
            map,
            kind,
            format,
            width,
            height,
        })
    }

    /// Closes this end, so a helper still waiting on it sees the channel end
    /// instead of waiting forever.
    pub(crate) fn close(self) {
        drop(self);
    }
}

/// One `memfd` holding a pixel buffer.
struct Memfd {
    fd: OwnedFd,
    len: usize,
}

impl Memfd {
    fn create(len: usize) -> Result<Self> {
        let name = c"vshot-pixels";
        // SAFETY: the name is a valid NUL-terminated string and no flags are
        // passed, so this only ever returns a fresh descriptor.
        let raw = unsafe { libc::memfd_create(name.as_ptr(), 0) };
        if raw < 0 {
            return Err(failed(format!(
                "failed to create a pixel buffer: {}",
                io::Error::last_os_error()
            )));
        }
        // SAFETY: the descriptor is freshly created and owned by nobody else.
        let fd = unsafe { OwnedFd::from_raw_fd(raw) };
        // `memfd_create` makes a file of length zero; the mapping needs it to
        // have the buffer's size before either side maps it.
        // SAFETY: the descriptor is open and writable.
        let rc = unsafe { libc::ftruncate(fd.as_raw_fd(), len as libc::off_t) };
        if rc != 0 {
            return Err(failed(format!(
                "failed to size the pixel buffer: {}",
                io::Error::last_os_error()
            )));
        }
        Ok(Self { fd, len })
    }

    fn raw(&self) -> RawFd {
        self.fd.as_raw_fd()
    }

    fn write_all(&self, bytes: &[u8]) -> Result<()> {
        debug_assert_eq!(bytes.len(), self.len);
        // SAFETY: the mapping covers `self.len` bytes, the descriptor is
        // writable and private to this process, and `bytes` is that long.
        let map = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                self.len,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                self.fd.as_raw_fd(),
                0,
            )
        };
        if map == libc::MAP_FAILED {
            return Err(failed(format!(
                "failed to fill the pixel buffer: {}",
                io::Error::last_os_error()
            )));
        }
        // SAFETY: the mapping is `self.len` bytes and does not overlap `bytes`,
        // which is a live slice of exactly that length.
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), map.cast::<u8>(), self.len);
            libc::munmap(map, self.len);
        }
        Ok(())
    }
}

/// Runs `command` with the pixel channel installed on [`CHILD_FD`].
pub(crate) fn with_channel(command: &mut Command, channel: &PixelChannel) {
    let fd = channel.fd.as_raw_fd();
    // SAFETY: `pre_exec` runs between `fork` and `exec`, so the closure may only
    // do async-signal-safe work -- and `dup2` is.  It touches no shared state
    // beyond the descriptor number captured by value, and `dup2` clears
    // `FD_CLOEXEC` on the copy, which is what makes this the one descriptor
    // that survives the exec.
    unsafe {
        command.pre_exec(move || {
            let rc = libc::dup2(fd, CHILD_FD);
            if rc < 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pixels(width: u32, height: u32, seed: u8) -> Vec<u8> {
        (0..width as usize * height as usize * 4)
            .map(|index| (index as u8).wrapping_add(seed))
            .collect()
    }

    #[test]
    fn a_buffer_round_trips_through_the_channel() {
        let (sender, receiver) = PixelChannel::pair().unwrap();
        let sent = pixels(4, 3, 7);
        sender
            .send(KIND_SOURCE, FORMAT_RGBA8888, 4, 3, &sent)
            .unwrap();
        let received = receiver.receive().unwrap();
        assert_eq!(received.kind, KIND_SOURCE);
        assert_eq!(received.format, FORMAT_RGBA8888);
        assert_eq!((received.width, received.height), (4, 3));
        assert_eq!(received.pixels(), sent.as_slice());
    }

    #[test]
    fn the_two_directions_do_not_collide() {
        // The helper answers on the same pair, so a source image and a result
        // are in flight over one channel; each side must get its own.
        let (here, there) = PixelChannel::pair().unwrap();
        let source = pixels(2, 2, 1);
        let result = pixels(2, 2, 200);
        here.send(KIND_SOURCE, FORMAT_RGBA8888, 2, 2, &source)
            .unwrap();
        there
            .send(KIND_RESULT, FORMAT_RGBA8888, 2, 2, &result)
            .unwrap();
        assert_eq!(there.receive().unwrap().pixels(), source.as_slice());
        let back = here.receive().unwrap();
        assert_eq!(back.kind, KIND_RESULT);
        assert_eq!(back.pixels(), result.as_slice());
    }

    #[test]
    fn a_buffer_of_the_wrong_length_is_refused_before_it_is_sent() {
        let (sender, _receiver) = PixelChannel::pair().unwrap();
        let error = sender
            .send(KIND_SOURCE, FORMAT_RGBA8888, 4, 3, &[0u8; 8])
            .unwrap_err();
        assert!(
            error.to_string().contains("expected 48"),
            "the message names the size it wanted: {error}"
        );
    }

    #[test]
    fn a_closed_channel_reports_the_end_rather_than_hanging() {
        let (here, there) = PixelChannel::pair().unwrap();
        there.close();
        let error = here.receive().unwrap_err();
        assert!(
            error.to_string().contains("closed"),
            "a dead helper reads as a closed channel: {error}"
        );
    }

    #[test]
    fn the_header_carries_the_size_and_the_kind() {
        let (sender, receiver) = PixelChannel::pair().unwrap();
        sender
            .send(KIND_RESULT, FORMAT_RGBA8888, 6, 5, &pixels(6, 5, 0))
            .unwrap();
        let received = receiver.receive().unwrap();
        assert_eq!((received.width, received.height), (6, 5));
        assert_eq!(received.kind, KIND_RESULT);
        assert_eq!(received.pixels().len(), 6 * 5 * 4);
    }
}
