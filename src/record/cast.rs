// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Reading a screen cast's PipeWire stream into a sink.
//!
//! Two routes produce a screen cast.  The XDG portal ([`super::portal`]) asks
//! `org.freedesktop.portal.ScreenCast` for one, with the compositor's own
//! picker deciding what it is; the compositor's own service
//! ([`super::screencast`], `org.gnome.Mutter.ScreenCast`) is aimed at a window
//! id directly and shows no picker at all.  Everything up to the stream differs
//! between them; everything from the first frame on is the same, and it lives
//! here so the two cannot drift apart in how they pace, convert or end a
//! recording.
//!
//! What the loop has to know about a screen cast:
//!
//! * **The frames are damage-driven.**  A cast produces a frame when what it
//!   is casting changes and nothing at all when it does not, so the loop waits
//!   for frames rather than sampling them; a still screen becomes a
//!   long-duration frame, which is exactly what was on screen.
//!
//! * **There are two frame shapes.**  A dma-buf frame goes to the GPU encoder
//!   without a copy; a memory frame is converted on the CPU.  Which one the
//!   stream produces is part of the negotiation ([`super::pipewire`] is the
//!   client that reads it).
//!
//! * **One stream, one layout.**  An MP4 holds one frame size and one pixel
//!   layout, so a cast that renegotiates mid-recording — a window resized, a
//!   monitor reconfigured — ends the recording cleanly rather than writing a
//!   file that contradicts itself.

use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use crate::error::{Result, VshotError};
use crate::geometry::Size;
use crate::model::Frame;

use super::avcodec::{AudioSink, VideoSink};
use super::debug_enabled;
use super::pipewire;

/// The `SPA_VIDEO_FORMAT_*` values a screen cast produces, and what vshot's
/// encoder can take.  The SPA name is the byte order in memory; `BGRx` is the
/// opaque layout a screen comes out as, `BGR` a window with alpha does.
pub(super) const SPA_RGBX: u32 = 7;
pub(super) const SPA_BGRX: u32 = 8;
pub(super) const SPA_RGBA: u32 = 11;
pub(super) const SPA_BGRA: u32 = 12;

/// How long the format negotiation may take before the stream is given up on.
/// The negotiation is a handful of round trips over the local socket, so this
/// only fires when something is genuinely wrong.
pub(super) const NEGOTIATION_TIMEOUT: Duration = Duration::from_secs(15);

/// How long the recording waits for its first frame.  The compositor queues
/// one as soon as the cast starts, so a stream that produces nothing — a
/// window on an output that is off, disabled or disconnected — is reported
/// after a few seconds instead of waited on forever.
pub(super) const FIRST_FRAME_TIMEOUT: Duration = Duration::from_secs(15);

/// How long one wait for a frame lasts before the loop looks around: at the
/// stop signal and at the `--duration` deadline.  A frame that arrives inside
/// the window is used immediately.
pub(super) const POLL: Duration = Duration::from_millis(200);

/// The frame shape the sink was opened for.  A cast's frames arrive in one of
/// these and cannot change without ending the recording: one MP4 holds one
/// pixel layout, exactly as it holds one frame size.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Shape {
    /// dma-buf frames the encoder imports directly (the zero-copy path).
    Dmabuf,
    /// Memory frames converted to RGBA on the CPU (the compatibility path).
    Software,
}

impl Shape {
    /// Which shape a stream that settled on `geometry` delivers.
    pub(super) fn of(geometry: &pipewire::Geometry) -> Self {
        if geometry.dmabuf {
            Shape::Dmabuf
        } else {
            Shape::Software
        }
    }

    /// What to call the shape in a debug trace.
    pub(super) fn word(self) -> &'static str {
        match self {
            Shape::Dmabuf => "dma-buf",
            Shape::Software => "memory",
        }
    }
}

/// The frame loop: wait for a frame, encode it with the time the previous one
/// was on screen, wait again.
///
/// `duration` is the `--duration` deadline, measured from the first frame
/// (everything before it is the negotiation, which on the portal route the
/// user may have spent answering a picker).  `control` is called at every
/// frame boundary, so a replay's save and stop requests are served with the
/// sink's state consistent; returning `true` ends the loop.
///
/// `fit_on_resize` decides what a cast that changes size mid-recording means.
/// One MP4 holds one frame size, so the change cannot be followed: `false`
/// ends the recording there, which is what a portal cast is worth — the
/// recording is of the screen the portal picked, and a screen that
/// reconfigures is a different one.  `true` fits the new frames into the
/// canvas the file was opened with (scaled down, centred, letterboxed), which
/// is what a *window* recording needs: the window was on screen the whole
/// time, and a file that ends the first time its user drags a corner is not a
/// recording of its session.
///
/// The frame rate is not here: `--fps` is the rate *asked of the compositor*
/// when the stream is opened, and what actually arrives is damage-driven, so
/// the loop paces itself off the arrivals instead.
#[allow(clippy::too_many_arguments)] // the stream's whole shape
pub(super) fn loop_over<S: VideoSink + AudioSink>(
    stream: &mut pipewire::Stream,
    sink: &mut S,
    duration: Option<u64>,
    shape: Shape,
    mut geometry: pipewire::Geometry,
    mic: &mut super::pipewire_audio::Soundtrack,
    interrupted: &AtomicBool,
    fit_on_resize: bool,
    mut control: impl FnMut(&mut S) -> Result<bool>,
) -> Result<()> {
    // The clock the recording's own length is measured against: the first
    // frame's arrival.  Everything before it is the negotiation, which the
    // user may have spent answering a picker.
    let mut started: Option<Instant> = None;
    let mut first_frame_deadline = Instant::now() + FIRST_FRAME_TIMEOUT;
    let mut last_frame_at = Instant::now();
    let mut timeline_ms = 0u64;
    let mut covered_us = 0u64;
    // What the last frame was, so the time between it and the stop signal can
    // be written as its duration once the loop is over: a frame's length is
    // only known when the next one arrives, and for the last one that is the
    // end of the recording.
    let mut tail_dmabuf: Option<pipewire::Dmabuf> = None;
    let mut tail_software: Option<Frame> = None;
    // How many frames this session has encoded, for the debug trace.
    let mut encoded = 0u64;

    loop {
        if interrupted.load(Ordering::Relaxed) {
            break;
        }
        if control(sink)? {
            break;
        }
        let now = Instant::now();
        if let Some(started) = started {
            if let Some(seconds) = duration {
                if started.elapsed() >= Duration::from_secs(seconds) {
                    break;
                }
            }
        } else if now >= first_frame_deadline {
            return Err(VshotError::Recording(format!(
                "the compositor's screen cast sent no frame within {} seconds; the source it is \
                 casting may be on an output that is off, disabled or disconnected",
                FIRST_FRAME_TIMEOUT.as_secs()
            )));
        }

        let frame = match stream.next(POLL) {
            Ok(Some(frame)) => frame,
            // A screen cast is damage-driven: no frame in the window is the
            // normal state of a screen nothing has happened on, not a failure.
            Ok(None) => continue,
            Err(error) => return Err(error),
        };
        let now = Instant::now();
        if started.is_none() {
            started = Some(now);
            last_frame_at = now;
            first_frame_deadline = now;
        }
        // A screen cast that renegotiates mid-recording — a window resized, a
        // monitor reconfigured — would have to change the layout of a file
        // that already has one, so the recording ends cleanly there instead.
        // The file up to that point is complete.
        let (frame_width, frame_height) = frame.size();
        if frame_width != geometry.width
            || frame_height != geometry.height
            || frame.spa_format() != geometry.spa_format
        {
            if !fit_on_resize {
                eprintln!(
                    "vshot: the recording ended: the screen cast changed to {frame_width}x\
                     {frame_height} (spa format {}), and one MP4 holds one frame size and layout",
                    frame.spa_format()
                );
                break;
            }
            // A window that was resized: the encoder is told to fit the new
            // frames into the canvas the file was opened with, and the
            // recording goes on.  The stream keeps delivering the new size
            // from here, so the loop's own idea of the shape moves with it.
            sink.resize_fit(frame_width, frame_height, fourcc_for(frame.spa_format())?)?;
            let (canvas_width, canvas_height) = sink.canvas();
            eprintln!(
                "vshot: the screen cast changed to {frame_width}x{frame_height}; fitting it into \
                 the recording's {canvas_width}x{canvas_height} canvas"
            );
            geometry.width = frame_width;
            geometry.height = frame_height;
            geometry.spa_format = frame.spa_format();
        }

        // The frame's duration is the time the *previous* frame was on
        // screen, which is the interval between the two arrivals — the same
        // accounting the screen and window loops use, so a cast recording
        // plays back on the same clock as the others.
        covered_us += now.saturating_duration_since(last_frame_at).as_micros() as u64;
        last_frame_at = now;
        let due_ms = covered_us / 1000;
        let step = due_ms.saturating_sub(timeline_ms);
        timeline_ms = timeline_ms.max(due_ms);
        let duration_ms = u32::try_from(step).unwrap_or(1).max(1);

        let encode_started = Instant::now();
        match shape {
            Shape::Dmabuf => {
                let Some(dmabuf) = frame.dmabuf() else {
                    eprintln!(
                        "vshot: the recording ended: the screen cast stopped sending dma-buf \
                         frames and one MP4 holds one pixel layout"
                    );
                    break;
                };
                sink.frame_dmabuf(
                    dmabuf.fd,
                    // The frame's own format, not the one the session opened
                    // with: a window that was resized may have changed its
                    // byte order too, and the encoder is told to fit that.
                    dmabuf.fourcc,
                    dmabuf.modifier,
                    dmabuf.offset,
                    dmabuf.stride,
                    duration_ms,
                )?;
                tail_dmabuf = Some(dmabuf);
            }
            Shape::Software => {
                let Some(memory) = frame.memory() else {
                    eprintln!(
                        "vshot: the recording ended: the screen cast stopped sending memory \
                         frames and one MP4 holds one pixel layout"
                    );
                    break;
                };
                let rgba = rgba_from_memory(
                    &memory,
                    geometry.width,
                    geometry.height,
                    geometry.spa_format,
                )?;
                sink.frame_rgba(rgba.pixels(), duration_ms)?;
                tail_software = Some(rgba);
            }
        }
        // The soundtrack for the interval this frame covered.
        super::pump_soundtrack(mic, sink)?;
        if debug_enabled() {
            encoded += 1;
            eprintln!(
                "vshot: frame {encoded}: muxed in {:.1}ms (on screen {duration_ms}ms, PipeWire \
                 sequence {})",
                encode_started.elapsed().as_secs_f64() * 1000.0,
                frame.sequence()
            );
        }
    }

    // The last interval has not been written yet, because a frame's length is
    // only known once the next frame arrives.  Sending the last frame a second
    // time with the time up to the end as its duration is what puts that time
    // in the file: without it a still screen's recording would be as long as
    // the moment of its last change, not as long as it was recorded.  The
    // client still holds the buffer, so its pixels are still there.
    // The soundtrack of the tail interval, queued before the last frame is
    // sent again: `finish` flushes the audio encoder after the video's, so
    // the samples have to be in by then.
    super::pump_soundtrack(mic, sink)?;
    let now = Instant::now();
    covered_us += now.saturating_duration_since(last_frame_at).as_micros() as u64;
    let step = (covered_us / 1000).saturating_sub(timeline_ms);
    if let Ok(duration_ms) = u32::try_from(step) {
        if duration_ms > 0 {
            match (&tail_dmabuf, &tail_software) {
                (Some(dmabuf), _) => sink.frame_dmabuf(
                    dmabuf.fd,
                    dmabuf.fourcc,
                    dmabuf.modifier,
                    dmabuf.offset,
                    dmabuf.stride,
                    duration_ms,
                )?,
                (None, Some(frame)) => sink.frame_rgba(frame.pixels(), duration_ms)?,
                (None, None) => {}
            }
        }
    }
    Ok(())
}

/// The DRM name for the byte order a `SPA_VIDEO_FORMAT_*` stands for, which is
/// what the encoder's dma-buf import takes.  Only the two layouts a screen
/// cast produces are known here; anything else is refused with the format
/// named rather than guessed at, because importing the wrong one gives a
/// file with the colours swapped and no error anywhere.
pub(super) fn fourcc_for(spa_format: u32) -> Result<u32> {
    match spa_format {
        SPA_BGRX => Ok(crate::capture::dmabuf::DRM_FORMAT_XRGB8888),
        SPA_BGRA => Ok(crate::capture::dmabuf::DRM_FORMAT_ARGB8888),
        other => Err(VshotError::Recording(format!(
            "the screen cast is sending pixels in SPA format {other}, which the encoder cannot \
             import; set VSHOT_PORTAL_SHM=1 to take the copy path instead"
        ))),
    }
}

/// Converts one memory frame to the packed RGBA the encoder's software path
/// takes, dropping the row padding and swapping the byte order.
///
/// The SPA name says what the bytes are; vshot works in RGBA, which is the
/// same four bytes with the red and blue ends swapped — the conversion the
/// wlroots capture path does for the same reason.  Doing it as whole 32-bit
/// words rather than a byte at a time is what lets it keep up with a full
/// screen; a copy is unavoidable here, because the memory belongs to the
/// compositor's buffer and the encoder wants a tight buffer of its own.
pub(super) fn rgba_from_memory(
    memory: &pipewire::Memory<'_>,
    width: u32,
    height: u32,
    spa_format: u32,
) -> Result<Frame> {
    let width = usize::try_from(width)
        .map_err(|_| VshotError::Recording("the frame width is too large".into()))?;
    let height = usize::try_from(height)
        .map_err(|_| VshotError::Recording("the frame height is too large".into()))?;
    let row_bytes = width
        .checked_mul(4)
        .ok_or_else(|| VshotError::Recording("the frame is too wide".into()))?;
    let expected = row_bytes
        .checked_mul(height)
        .ok_or_else(|| VshotError::Recording("the frame is too large".into()))?;
    if memory.stride < row_bytes {
        return Err(VshotError::Recording(format!(
            "the screen cast's frame has a stride of {} bytes, which is less than one row of \
             {width} pixels",
            memory.stride
        )));
    }
    if memory.bytes.len() < memory.stride * height {
        return Err(VshotError::Recording(format!(
            "the screen cast's frame has {} bytes, which is less than {height} rows of {} bytes",
            memory.bytes.len(),
            memory.stride
        )));
    }

    // What the bytes mean: whether the red and blue ends are swapped, and what
    // to do about a fourth byte that is not alpha.
    let (swap, opaque) = match spa_format {
        SPA_BGRX => (true, true),
        SPA_BGRA => (true, false),
        SPA_RGBX => (false, true),
        SPA_RGBA => (false, false),
        other => {
            return Err(VshotError::Recording(format!(
                "the screen cast is sending pixels in SPA format {other}, which this path cannot \
                 convert"
            )))
        }
    };

    let mut pixels = vec![0u8; expected];
    for (destination_row, source_row) in pixels.chunks_exact_mut(row_bytes).zip(
        memory
            .bytes
            .chunks_exact(memory.stride.max(row_bytes))
            .take(height),
    ) {
        for (destination, source) in destination_row
            .chunks_exact_mut(4)
            .zip(source_row[..row_bytes].chunks_exact(4))
        {
            let word = u32::from_le_bytes([source[0], source[1], source[2], source[3]]);
            let word = if swap {
                (word & 0x0000_00FF) << 16 | (word & 0x00FF_0000) >> 16 | (word & 0xFF00_FF00)
            } else {
                word
            };
            let word = if opaque { word | 0xFF00_0000 } else { word };
            destination.copy_from_slice(&word.to_le_bytes());
        }
    }
    Frame::new(Size::new(width as u32, height as u32), pixels)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The two layouts a screen cast produces map onto the two DRM names the
    /// encoder imports; anything else has to be refused, because a wrong
    /// mapping is a valid file with the colours swapped.
    #[test]
    fn the_formats_the_encoder_imports_are_the_ones_known() {
        assert_eq!(
            fourcc_for(SPA_BGRX).unwrap(),
            crate::capture::dmabuf::DRM_FORMAT_XRGB8888
        );
        assert_eq!(
            fourcc_for(SPA_BGRA).unwrap(),
            crate::capture::dmabuf::DRM_FORMAT_ARGB8888
        );
        for unknown in [SPA_RGBX, SPA_RGBA, 0, 999] {
            let error = fourcc_for(unknown).unwrap_err().to_string();
            assert!(
                error.contains(&unknown.to_string()),
                "the refusal has to name the format: {error}"
            );
        }
    }

    /// A memory frame becomes vshot's RGBA: BGRx is the layout a screen comes
    /// out as, and the byte that is not alpha has to come out opaque.
    #[test]
    fn a_bgrx_frame_becomes_rgba() {
        // Two pixels: one blue-ish, one with a zero fourth byte.
        let bytes = vec![10, 20, 30, 0, 40, 50, 60, 0];
        let memory = pipewire::Memory {
            bytes: &bytes,
            stride: 8,
        };
        let frame = rgba_from_memory(&memory, 2, 1, SPA_BGRX).unwrap();
        assert_eq!(frame.pixels(), &[30, 20, 10, 255, 60, 50, 40, 255]);
        assert_eq!(frame.size(), Size::new(2, 1));
    }

    /// BGRA keeps its alpha: a window recording has to stay see-through where
    /// the window is.
    #[test]
    fn a_bgra_frame_keeps_its_alpha() {
        let bytes = vec![10, 20, 30, 40];
        let memory = pipewire::Memory {
            bytes: &bytes,
            stride: 4,
        };
        let frame = rgba_from_memory(&memory, 1, 1, SPA_BGRA).unwrap();
        assert_eq!(frame.pixels(), &[30, 20, 10, 40]);
    }

    /// The rows of a memory frame can be padded; the padding must not reach
    /// the encoder, which takes tight rows.
    #[test]
    fn row_padding_is_dropped() {
        let bytes = vec![
            1, 2, 3, 4, 0xAA, 0xBB, 0xCC, 0xDD, 5, 6, 7, 8, 9, 10, 11, 12,
        ];
        let memory = pipewire::Memory {
            bytes: &bytes,
            stride: 8,
        };
        let frame = rgba_from_memory(&memory, 1, 2, SPA_BGRA).unwrap();
        assert_eq!(frame.pixels(), &[3, 2, 1, 4, 7, 6, 5, 8]);
    }

    /// A frame that is too small for the stride it claims is refused rather
    /// than read past its end.
    #[test]
    fn a_short_frame_is_refused() {
        let bytes = vec![0u8; 4];
        let memory = pipewire::Memory {
            bytes: &bytes,
            stride: 8,
        };
        assert!(rgba_from_memory(&memory, 2, 1, SPA_BGRX).is_err());
        let memory = pipewire::Memory {
            bytes: &bytes,
            stride: 4,
        };
        assert!(rgba_from_memory(&memory, 2, 1, SPA_BGRX).is_err());
    }

    /// The shape a stream settled on is what decides which way its frames
    /// travel: a dma-buf goes to the encoder as it is, memory is converted.
    #[test]
    fn the_shape_follows_the_streams_negotiation() {
        let dmabuf = pipewire::Geometry {
            width: 1920,
            height: 1080,
            spa_format: SPA_BGRX,
            dmabuf: true,
        };
        let memory = pipewire::Geometry {
            dmabuf: false,
            ..dmabuf
        };
        assert_eq!(Shape::of(&dmabuf), Shape::Dmabuf);
        assert_eq!(Shape::of(&memory), Shape::Software);
        assert_eq!(Shape::Dmabuf.word(), "dma-buf");
        assert_eq!(Shape::Software.word(), "memory");
    }
}
