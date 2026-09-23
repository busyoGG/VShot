//! One window's own pixels, through `ext_image_copy_capture_v1`.
//!
//! The screenshots read a *window's rectangle* off the scene: compositor
//! metadata says where the window is (`window.rs`) and the rectangle is cut out
//! of a screen capture.  That is not the same thing as capturing a window, and
//! the difference is the whole reason this module exists: an occluded window
//! comes out as the window covering it, and a window that moves comes out at
//! the place it used to be.  A window's own pixels can only come from the
//! compositor handing them over, and the route for that is a capture source
//! built from the window's foreign-toplevel handle.
//!
//! Both protocols are staging extensions — `ext_foreign_toplevel_list_v1` to
//! name the windows, `ext_image_copy_capture_v1` to copy one of them — and a
//! session without them is told exactly what is missing rather than quietly
//! falling back to a rectangle that would be captured wrongly.
//!
//! The shape of a session: enumerate the toplevels, take one handle, build a
//! capture source from it, ask the capture manager for a session, and read the
//! buffer constraints the session sends (`buffer_size`, and `dmabuf_format`
//! with the modifiers the compositor can import).  Unlike screencopy, this
//! protocol has the *client* allocate the buffer the compositor copies into,
//! so the pool here is allocated from those constraints; the alternative is a
//! buffer the compositor refuses.
//!
//! The compositor may hold a copy until the window's content changes, which is
//! why [`WindowCapture::grab`] never blocks for longer than the caller's frame
//! interval: it answers [`Capture::Idle`] and leaves the request in flight, so
//! a still window costs one long frame rather than the loop's liveness.

use std::os::fd::BorrowedFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use wayland_client::protocol::{wl_buffer, wl_registry};
use wayland_client::{Connection, Dispatch, EventQueue, QueueHandle, WEnum};
use wayland_protocols::ext::foreign_toplevel_list::v1::client::{
    ext_foreign_toplevel_handle_v1, ext_foreign_toplevel_list_v1,
};
use wayland_protocols::ext::image_capture_source::v1::client::{
    ext_foreign_toplevel_image_capture_source_manager_v1, ext_image_capture_source_v1,
};
use wayland_protocols::ext::image_copy_capture::v1::client::{
    ext_image_copy_capture_frame_v1, ext_image_copy_capture_manager_v1,
    ext_image_copy_capture_session_v1,
};
use wayland_protocols::wp::linux_dmabuf::zv1::client::{
    zwp_linux_buffer_params_v1, zwp_linux_dmabuf_v1,
};

use crate::error::{Result, VshotError};

use super::dmabuf::{DmabufFrame, GbmBuffer, DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888};
use super::window::join_label;

/// How long the compositor gets to describe a session's buffers.  They arrive
/// in one batch right after the session is created, so this is a bound on
/// "something is wrong" rather than a wait anyone should ever sit through.
const CONSTRAINTS_TIMEOUT: Duration = Duration::from_secs(10);

/// Slots in the dma-buf pool: four buffers the size of the window.  One is
/// being copied into while the encoder still reads the previous ones, and a
/// window's buffer is far smaller than a 4K output's, so four is cheap.
const POOL_SLOTS: usize = 4;

/// How long one poll waits before the loop is given a chance to notice a stop
/// signal and the `--duration` deadline.
const POLL_GRANULARITY: Duration = Duration::from_millis(100);

/// The three failure codes a capture frame can carry, from the protocol's
/// `failure_reason` enum.  They are compared as numbers so a compositor that
/// sends a code this build does not know still lands in the right branch.
const FAILED_UNKNOWN: u32 = 0;
const FAILED_BUFFER_CONSTRAINTS: u32 = 1;
const FAILED_STOPPED: u32 = 2;

/// A window's own labels.  Both may be empty (an XWayland window without a
/// class, a client that never set a title), and the identifier is the
/// compositor's stable way of pointing at the same window twice.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Name {
    pub app_id: String,
    pub title: String,
    pub identifier: String,
}

impl Name {
    /// `class — title`, the way a compositor's own window list names a window.
    pub fn label(&self) -> String {
        join_label(&self.app_id, &self.title)
    }

    /// The label, or a stand-in when the compositor reported neither.
    fn describe(&self) -> String {
        let label = self.label();
        if label.is_empty() {
            "(the compositor reports no app id or title)".to_owned()
        } else {
            label
        }
    }
}

/// One window the compositor lists: the handle a capture source needs, and the
/// labels the user recognises it by.
pub struct Toplevel {
    handle: ext_foreign_toplevel_handle_v1::ExtForeignToplevelHandleV1,
    pub name: Name,
}

impl Toplevel {
    pub fn label(&self) -> String {
        self.name.label()
    }
}

/// The frame shape a session settled on: what the encoder has to be opened for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Shape {
    pub width: u32,
    pub height: u32,
    pub fourcc: u32,
}

/// What one [`WindowCapture::grab`] produced.
pub enum Capture {
    /// The compositor copied the window into one of the pool's dma-bufs.
    Frame(DmabufFrame),
    /// Nothing arrived within the wait: the compositor is holding the copy
    /// until the window's content changes.  The request stays in flight, so the
    /// next call picks up the same frame.
    Idle,
    /// The session is over; the message says why, for the recording to report.
    Ended(String),
    /// The caller asked to stop.
    Interrupted,
}

/// One pooled dma-buf with the `wl_buffer` the compositor copies into.
struct Slot {
    gbm: GbmBuffer,
    wl_buffer: wl_buffer::WlBuffer,
}

impl std::fmt::Debug for Slot {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("Slot")
            .field("fd", &self.gbm.fd())
            .finish_non_exhaustive()
    }
}

impl Drop for Slot {
    fn drop(&mut self) {
        self.wl_buffer.destroy();
    }
}

/// A toplevel the compositor has told us about, with its properties as they
/// stood at the last `done`.
struct Listed {
    handle: ext_foreign_toplevel_handle_v1::ExtForeignToplevelHandleV1,
    name: Name,
    done: bool,
    closed: bool,
}

/// The frame in flight: what the compositor is copying right now.
struct Pending {
    frame: ext_image_copy_capture_frame_v1::ExtImageCopyCaptureFrameV1,
    slot: usize,
    ready: bool,
    failed: Option<u32>,
}

/// A live capture session: the compositor's session object, the pool built for
/// its constraints, and whatever frame is in flight.
struct Live {
    session: ext_image_copy_capture_session_v1::ExtImageCopyCaptureSessionV1,
    _source: ext_image_capture_source_v1::ExtImageCaptureSourceV1,
    buffer_size: Option<(u32, u32)>,
    formats: Vec<(u32, Vec<u64>)>,
    /// The `dev_t` the compositor says buffers must be allocated on, as it sent
    /// it (a raw byte array).  Read for the one diagnostic it enables: a
    /// session with two GPUs (a render node for the encoder, a display GPU for
    /// the compositor) fails in the driver's EGL import, and naming the
    /// mismatch is what turns "the buffer could not be imported" into an
    /// answer.
    dmabuf_device: Vec<u8>,
    constraints_done: bool,
    stopped: bool,
    /// Set by the `failed` event of a `zwp_linux_buffer_params_v1` this session
    /// created: the compositor could not import the dma-buf it was handed.
    params_failed: bool,
    slots: Vec<Slot>,
    next: usize,
    shape: Option<Shape>,
    /// The slot the last delivered frame lives in.  Held out of rotation in
    /// the sense that it is only overwritten by a later *successful* frame
    /// (which then becomes the last one), so the recording's tail can re-send
    /// it: see [`WindowCapture::last_frame`].
    last_slot: Option<usize>,
    /// When the session started waiting for its first frame.  A window whose
    /// output never commits — a disabled or disconnected monitor — never gets
    /// one, and that has to be reported rather than waited on forever.
    first_wait_started: Instant,
    pending: Option<Pending>,
}

/// The protocol state of one connection to the compositor.
#[derive(Default)]
struct CopyState {
    dmabuf: Option<zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1>,
    list: Option<ext_foreign_toplevel_list_v1::ExtForeignToplevelListV1>,
    listed: Vec<Listed>,
    source_manager:
        Option<ext_foreign_toplevel_image_capture_source_manager_v1::ExtForeignToplevelImageCaptureSourceManagerV1>,
    manager: Option<ext_image_copy_capture_manager_v1::ExtImageCopyCaptureManagerV1>,
    live: Option<Live>,
}

/// The window capture client: one Wayland connection of its own, because the
/// screencopy connection a recording may also hold is a different object graph.
pub struct WindowCapture {
    event_queue: EventQueue<CopyState>,
    state: CopyState,
}

impl WindowCapture {
    /// Connects and binds the protocols window capture needs.  A compositor
    /// without them is reported here, before any recording file is created.
    pub fn connect() -> Result<Self> {
        let connection = Connection::connect_to_env()
            .map_err(|error| VshotError::WaylandConnection(error.to_string()))?;
        let mut event_queue = connection.new_event_queue::<CopyState>();
        let qh = event_queue.handle();
        let mut state = CopyState::default();
        connection.display().get_registry(&qh, ());
        event_queue
            .roundtrip(&mut state)
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        event_queue
            .roundtrip(&mut state)
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        for missing in [
            (state.list.is_none(), "ext_foreign_toplevel_list_v1"),
            (
                state.source_manager.is_none(),
                "ext_foreign_toplevel_image_capture_source_manager_v1",
            ),
            (state.manager.is_none(), "ext_image_copy_capture_manager_v1"),
        ] {
            if missing.0 {
                return Err(VshotError::Recording(format!(
                    "this compositor does not offer window capture (`{}`); record a screen instead \
                     (`vshot record monitor`, or `vshot record all`)",
                    missing.1
                )));
            }
        }
        if state.dmabuf.is_none() {
            return Err(VshotError::MissingCapability("zwp_linux_dmabuf_v1".into()));
        }
        Ok(Self { event_queue, state })
    }

    /// Every mapped window the compositor lists.  The list is enumerated on the
    /// connection's first roundtrip, so this is the set of windows that existed
    /// when the capture client connected; a window that closed since is
    /// dropped.
    pub fn toplevels(&mut self) -> Result<Vec<Toplevel>> {
        // The properties of each handle arrive right after its `toplevel`
        // event, closed off by that handle's `done`, so two roundtrips are
        // enough for the whole list to have settled.  (One would do for the
        // events the bind already queued; the second is what makes the answer
        // independent of when the compositor flushed them.)
        for _ in 0..2 {
            self.event_queue
                .roundtrip(&mut self.state)
                .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        }
        let complete = self.state.listed.iter().any(|listed| listed.done);
        Ok(self
            .state
            .listed
            .iter()
            .filter(|listed| !listed.closed && (listed.done || !complete))
            .map(|listed| Toplevel {
                handle: listed.handle.clone(),
                name: listed.name.clone(),
            })
            .collect())
    }

    /// Starts a session for one window and builds the buffer pool from the
    /// constraints the compositor sends.  The shape comes back so the encoder
    /// can be opened for it; nothing is captured until [`Self::grab`] runs.
    pub fn start(&mut self, toplevel: &Toplevel) -> Result<Shape> {
        if self.state.live.is_some() {
            return Err(VshotError::WaylandProtocol(
                "a window capture session is already running".into(),
            ));
        }
        let qh = self.event_queue.handle();
        let source_manager = self.state.source_manager.clone().ok_or_else(|| {
            VshotError::Recording(
                "this compositor does not offer window capture \
                 (`ext_foreign_toplevel_image_capture_source_manager_v1`)"
                    .into(),
            )
        })?;
        let manager = self.state.manager.clone().ok_or_else(|| {
            VshotError::Recording(
                "this compositor does not offer window capture (`ext_image_copy_capture_manager_v1`)"
                    .into(),
            )
        })?;
        let dmabuf = self
            .state
            .dmabuf
            .clone()
            .ok_or_else(|| VshotError::MissingCapability("zwp_linux_dmabuf_v1".into()))?;

        let source = source_manager.create_source(&toplevel.handle, &qh, ());
        // No cursor is ever painted into a window's own pixels: the pointer is
        // not part of the window, so the option has nothing to ask for here.
        let session = manager.create_session(
            &source,
            ext_image_copy_capture_manager_v1::Options::empty(),
            &qh,
            (),
        );
        self.state.live = Some(Live {
            session,
            _source: source,
            buffer_size: None,
            formats: Vec::new(),
            dmabuf_device: Vec::new(),
            constraints_done: false,
            stopped: false,
            params_failed: false,
            slots: Vec::new(),
            next: 0,
            shape: None,
            last_slot: None,
            first_wait_started: Instant::now(),
            pending: None,
        });

        let deadline = Instant::now() + CONSTRAINTS_TIMEOUT;
        let wait = self.wait_for(deadline, None, |state| {
            state
                .live
                .as_ref()
                .is_some_and(|live| live.constraints_done || live.stopped)
        })?;
        let Some(live) = self.state.live.as_mut() else {
            return Err(VshotError::WaylandProtocol(
                "the window capture session disappeared while it was being set up".into(),
            ));
        };
        if live.stopped {
            return Err(VshotError::Recording(
                "the compositor stopped the window capture session as soon as it was created"
                    .into(),
            ));
        }
        if !matches!(wait, Wait::Done) {
            return Err(VshotError::Recording(
                "the compositor did not describe the window capture buffers in time".into(),
            ));
        }
        let (width, height) = live.buffer_size.ok_or_else(|| {
            VshotError::Recording("the window capture session reports no buffer size".into())
        })?;
        let (fourcc, modifiers) = pick_format(&live.formats).ok_or_else(|| {
            VshotError::Recording(format!(
                "the compositor offers no dma-buf format this encoder can take for this window \
                 (it offered {}); the GPU path needs ARGB8888 or XRGB8888",
                describe_formats(&live.formats)
            ))
        })?;
        // `create_immed` can fail on the compositor's side without saying so:
        // the protocol's answer is the `failed` event, and a buffer whose
        // creation failed is simply not there — the next `attach_buffer` is
        // what reports it, as an object the server does not know.  The event is
        // watched here so that failure can be named at the point it happened.
        let mut create_failed = false;
        for _ in 0..POOL_SLOTS {
            let gbm = GbmBuffer::create_with_modifiers(width, height, fourcc, &modifiers)?;
            let params = dmabuf.create_params(&qh, ());
            let borrowed = unsafe { BorrowedFd::borrow_raw(gbm.fd()) };
            params.add(
                borrowed,
                0,
                gbm.offset(),
                gbm.stride(),
                (gbm.modifier() >> 32) as u32,
                (gbm.modifier() & 0xffff_ffff) as u32,
            );
            let wl_buffer = params.create_immed(
                i32::try_from(width).map_err(|_| {
                    VshotError::Recording("the window is too wide to capture".into())
                })?,
                i32::try_from(height).map_err(|_| {
                    VshotError::Recording("the window is too tall to capture".into())
                })?,
                fourcc,
                zwp_linux_buffer_params_v1::Flags::empty(),
                &qh,
                (),
            );
            params.destroy();
            // The `failed` event is dispatched with the next roundtrip; one is
            // enough because it is sent in answer to the request just made.
            if self
                .event_queue
                .roundtrip(&mut self.state)
                .map_err(|error| VshotError::WaylandProtocol(error.to_string()))
                .is_err()
            {
                create_failed = true;
                break;
            }
            let Some(live) = self.state.live.as_mut() else {
                return Err(VshotError::WaylandProtocol(
                    "the window capture session disappeared while its pool was being built".into(),
                ));
            };
            if live.params_failed {
                create_failed = true;
                break;
            }
            live.slots.push(Slot { gbm, wl_buffer });
        }
        let Some(live) = self.state.live.as_mut() else {
            return Err(VshotError::WaylandProtocol(
                "the window capture session disappeared while its pool was being built".into(),
            ));
        };
        if create_failed {
            return Err(VshotError::Recording(format!(
                "the compositor could not import a {width}x{height} dma-buf (fourcc \
                 0x{fourcc:08x}, modifier 0x{:x}), which means the modifier it advertised is not \
                 one it can actually take",
                modifiers.first().copied().unwrap_or(0)
            )));
        }
        let shape = Shape {
            width,
            height,
            fourcc,
        };
        live.shape = Some(shape);
        if debug_enabled() {
            eprintln!(
                "vshot: window capture session {width}x{height}, fourcc 0x{fourcc:08x}, {} \
                 modifier(s) offered ({}), {} buffer(s) allocated at modifier 0x{:x}",
                modifiers.len(),
                modifiers
                    .iter()
                    .map(|modifier| format!("0x{modifier:x}"))
                    .collect::<Vec<_>>()
                    .join(" "),
                POOL_SLOTS,
                live.slots
                    .first()
                    .map(|slot| slot.gbm.modifier())
                    .unwrap_or(0)
            );
            for (format, modifiers) in &live.formats {
                eprintln!(
                    "vshot:   offered 0x{format:08x}: {}",
                    modifiers
                        .iter()
                        .map(|modifier| format!("0x{modifier:x}"))
                        .collect::<Vec<_>>()
                        .join(" ")
                );
            }
            eprintln!(
                "vshot:   compositor device dev_t bytes: {}",
                live.dmabuf_device
                    .iter()
                    .map(|byte| format!("{byte:02x}"))
                    .collect::<Vec<_>>()
                    .join("")
            );
        }
        Ok(shape)
    }

    /// Asks for one frame and waits up to `wait` for it.  A frame that does not
    /// arrive in time is not lost: it stays in flight and the next call picks
    /// it up (see [`Capture::Idle`]).
    pub fn grab(&mut self, wait: Duration, interrupted: &AtomicBool) -> Result<Capture> {
        if interrupted.load(Ordering::Relaxed) {
            return Ok(Capture::Interrupted);
        }
        let Some(live) = self.state.live.as_mut() else {
            return Err(VshotError::WaylandProtocol(
                "no window capture session is running".into(),
            ));
        };
        if live.stopped {
            return Ok(Capture::Ended(
                "the compositor stopped the window capture (the window may have been closed)"
                    .to_owned(),
            ));
        }
        let Some(shape) = live.shape else {
            return Err(VshotError::WaylandProtocol(
                "no window capture session is running".into(),
            ));
        };
        // A window that changed size changes the size of the frames the
        // compositor sends, and a recording is one fixed frame size from the
        // first packet to the trailer: the file ends here rather than growing
        // frames the encoder would refuse.
        if live.buffer_size != Some((shape.width, shape.height)) {
            let (width, height) = live.buffer_size.unwrap_or((shape.width, shape.height));
            return Ok(Capture::Ended(format!(
                "the window changed size, from {}x{} to {width}x{height}, which one recording \
                 cannot follow",
                shape.width, shape.height
            )));
        }
        if live.pending.is_none() {
            let qh = self.event_queue.handle();
            let slot = live.next;
            let frame = live.session.create_frame(&qh, ());
            let Some(pooled) = live.slots.get(slot) else {
                return Err(VshotError::WaylandProtocol(
                    "the window capture pool is empty".into(),
                ));
            };
            frame.attach_buffer(&pooled.wl_buffer);
            // Every frame damages the whole buffer: the compositor may use the
            // damage to copy less, and a client that does not track damage is
            // told to damage everything.
            frame.damage_buffer(
                0,
                0,
                i32::try_from(shape.width).unwrap_or(i32::MAX),
                i32::try_from(shape.height).unwrap_or(i32::MAX),
            );
            frame.capture();
            live.pending = Some(Pending {
                frame,
                slot,
                ready: false,
                failed: None,
            });
        }

        let wait = self.wait_for(Instant::now() + wait, Some(interrupted), |state| {
            state
                .live
                .as_ref()
                .and_then(|live| live.pending.as_ref())
                .is_some_and(|pending| pending.ready || pending.failed.is_some())
        })?;
        if matches!(wait, Wait::Interrupted) {
            return Ok(Capture::Interrupted);
        }
        if !matches!(wait, Wait::Done) {
            return Ok(Capture::Idle);
        }

        let Some(live) = self.state.live.as_mut() else {
            return Err(VshotError::WaylandProtocol(
                "the window capture session disappeared mid-frame".into(),
            ));
        };
        let Some(pending) = live.pending.take() else {
            return Ok(Capture::Idle);
        };
        pending.frame.destroy();
        if let Some(code) = pending.failed {
            return match code {
                FAILED_STOPPED => Ok(Capture::Ended(
                    "the compositor stopped the window capture (the window may have been closed)"
                        .to_owned(),
                )),
                FAILED_BUFFER_CONSTRAINTS => Ok(Capture::Ended(
                    "the compositor refused the capture buffer: the window's buffers no longer \
                     match the shape they were allocated for"
                        .to_owned(),
                )),
                // An unnamed runtime error, which the protocol says may be
                // retried: a dropped frame rather than the end of the
                // recording.
                _ => Err(VshotError::Recording(format!(
                    "the compositor could not copy this window (capture failure {code})"
                ))),
            };
        }
        if !pending.ready {
            return Err(VshotError::WaylandProtocol(
                "the window capture frame ended without a result".into(),
            ));
        }
        let Some(pooled) = live.slots.get(pending.slot) else {
            return Err(VshotError::WaylandProtocol(
                "the window capture pool slot vanished mid-frame".into(),
            ));
        };
        live.last_slot = Some(pending.slot);
        live.next = (pending.slot + 1) % live.slots.len().max(1);
        let gbm = &pooled.gbm;
        Ok(Capture::Frame(DmabufFrame {
            fd: gbm.fd(),
            fourcc: gbm.fourcc(),
            modifier: gbm.modifier(),
            offset: gbm.offset(),
            stride: gbm.stride(),
            width: gbm.width(),
            height: gbm.height(),
        }))
    }

    /// The last frame the compositor delivered, as a descriptor, for a
    /// recording to send one final time on its way out.
    ///
    /// A recording's timeline is the wallclock its frames were on screen, and
    /// the last interval it knows about is the one that was still running when
    /// the stop signal arrived.  Re-sending the last frame's pixels with that
    /// interval as their duration is what puts that time into the file; the
    /// pixels are the ones that were on screen for it.
    pub fn last_frame(&self) -> Option<DmabufFrame> {
        let live = self.state.live.as_ref()?;
        let slot = live.slots.get(live.last_slot?)?;
        let gbm = &slot.gbm;
        Some(DmabufFrame {
            fd: gbm.fd(),
            fourcc: gbm.fourcc(),
            modifier: gbm.modifier(),
            offset: gbm.offset(),
            stride: gbm.stride(),
            width: gbm.width(),
            height: gbm.height(),
        })
    }

    /// Whether the compositor has delivered any frame at all yet.  A window
    /// whose output never commits (a disabled or disconnected monitor) never
    /// gets one, which [`Self::first_frame_overdue`] turns into an error.
    pub fn has_frames(&self) -> bool {
        self.state
            .live
            .as_ref()
            .is_some_and(|live| live.last_slot.is_some())
    }

    /// How long this session has been waiting for its first frame.
    pub fn first_frame_wait(&self) -> Duration {
        self.state
            .live
            .as_ref()
            .map(|live| live.first_wait_started.elapsed())
            .unwrap_or_default()
    }

    /// Dispatches until `done` holds, `interrupted` is set, or `deadline`
    /// passes.  Polling in short slices is what keeps a stop signal and the
    /// `--duration` deadline live while a compositor holds a frame back.
    fn wait_for(
        &mut self,
        deadline: Instant,
        interrupted: Option<&AtomicBool>,
        done: impl Fn(&CopyState) -> bool,
    ) -> Result<Wait> {
        loop {
            self.event_queue
                .dispatch_pending(&mut self.state)
                .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
            if done(&self.state) {
                return Ok(Wait::Done);
            }
            if interrupted.is_some_and(|flag| flag.load(Ordering::Relaxed)) {
                return Ok(Wait::Interrupted);
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Ok(Wait::Timeout);
            }
            self.event_queue
                .flush()
                .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
            let Some(read_guard) = self.event_queue.prepare_read() else {
                continue;
            };
            let fd = read_guard.connection_fd();
            let mut poll_fds = [rustix::event::PollFd::new(
                &fd,
                rustix::event::PollFlags::IN | rustix::event::PollFlags::ERR,
            )];
            let timeout = rustix::event::Timespec::try_from(remaining.min(POLL_GRANULARITY))
                .map_err(|_| VshotError::WaylandProtocol("poll timeout is out of range".into()))?;
            let ready = rustix::event::poll(&mut poll_fds, Some(&timeout)).map_err(|error| {
                VshotError::WaylandProtocol(format!(
                    "failed to poll the Wayland connection: {error}"
                ))
            })?;
            if ready == 0 {
                drop(read_guard);
                continue;
            }
            read_guard
                .read()
                .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        }
    }
}

/// How a wait ended.
enum Wait {
    Done,
    Timeout,
    Interrupted,
}

/// Whether the tracing the recording does with `VSHOT_RECORD_DEBUG=1` is on.
fn debug_enabled() -> bool {
    std::env::var_os("VSHOT_RECORD_DEBUG").is_some()
}

/// The DRM modifier of a plainly laid-out buffer, and the one that means "no
/// modifier stated" — a compositor may list the second among its offers, and it
/// is not something that can be handed to `wl_buffer`.
const DRM_FORMAT_MOD_LINEAR: u64 = 0;
const DRM_FORMAT_MOD_INVALID: u64 = 0x00ff_ffff_ffff_ffff;

/// The dma-buf format to allocate this window's buffers in, with the modifiers
/// that go with it.  Only ARGB8888 and XRGB8888 have a path through the shim to
/// a VAAPI surface (see its `fourcc_to_sw_format`), so one of the two has to be
/// among what the compositor offered; XRGB8888 comes first because a recording
/// has no alpha channel to keep.
fn pick_format(formats: &[(u32, Vec<u64>)]) -> Option<(u32, Vec<u64>)> {
    [DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888]
        .into_iter()
        .find_map(|wanted| {
            formats
                .iter()
                .find(|(format, _)| *format == wanted)
                .map(|(_, modifiers)| (wanted, order_modifiers(modifiers)))
                .filter(|(_, modifiers)| !modifiers.is_empty())
        })
}

/// The offered modifiers in the order to try them: linear, and *only* linear
/// when it is on offer.
///
/// Two traps meet here, and both were measured on this machine.
///
/// The first is that a compositor offering a tiled modifier is not the same as
/// the compositor being able to import one.  Hyprland with AMD's DCC modifiers
/// first in its list accepts the buffer at `attach_buffer` and then fails the
/// copy with `eglCreateImageKHR: EGL_BAD_MATCH`, which is too late to recover
/// from — a protocol error ends the connection.
///
/// The second is that GBM ignores the order of the list it is handed: given
/// `[linear, dcc]` it allocates a DCC buffer anyway (radeonsi picks what it
/// likes when it is not forced), so asking for "linear first" is not enough.
/// Handing it a list of one is.  Linear is what the screencopy pool has always
/// allocated here and what the encoder's VAAPI import is known to take, so when
/// the compositor offers it — and it offers everything it can import — that is
/// the whole list.  A compositor that does not offer linear gets its own list
/// back, minus the "no modifier stated" entry that no buffer can be built on.
fn order_modifiers(modifiers: &[u64]) -> Vec<u64> {
    if modifiers.contains(&DRM_FORMAT_MOD_LINEAR) {
        return vec![DRM_FORMAT_MOD_LINEAR];
    }
    modifiers
        .iter()
        .copied()
        .filter(|modifier| *modifier != DRM_FORMAT_MOD_INVALID)
        .collect()
}

/// What the compositor offered, for an error message that has to say what it
/// saw instead of the format it wanted.
fn describe_formats(formats: &[(u32, Vec<u64>)]) -> String {
    if formats.is_empty() {
        return "no dma-buf format at all".to_owned();
    }
    formats
        .iter()
        .map(|(format, modifiers)| format!("0x{format:08x} ({} modifier(s))", modifiers.len()))
        .collect::<Vec<_>>()
        .join(", ")
}

/// The `modifiers` array as the 64-bit values it is: Wayland carries arrays as
/// bytes, and the protocol's own description is an array of `uint64_t`.
fn modifiers_from_bytes(bytes: &[u8]) -> Vec<u64> {
    let mut modifiers = Vec::with_capacity(bytes.len() / 8);
    for chunk in bytes.chunks_exact(8) {
        let mut value = [0u8; 8];
        value.copy_from_slice(chunk);
        modifiers.push(u64::from_ne_bytes(value));
    }
    modifiers
}

/// The window a filter names, as an index into `names`.
///
/// An exact app id or title wins, because that is what a user who typed the
/// whole name means; otherwise the filter is a case-insensitive substring of
/// either.  Several matches are an error that lists them: recording a window
/// the user did not mean is worse than asking again.
pub fn select(names: &[&Name], filter: &str) -> Result<usize> {
    let filter = filter.trim();
    if filter.is_empty() {
        return Err(VshotError::Recording(
            "the window filter is empty: give an app id or a title, or use `--pick`".into(),
        ));
    }
    let exact: Vec<usize> = names
        .iter()
        .enumerate()
        .filter(|(_, name)| {
            name.app_id.eq_ignore_ascii_case(filter) || name.title.eq_ignore_ascii_case(filter)
        })
        .map(|(index, _)| index)
        .collect();
    let matches = if exact.is_empty() {
        let lowered = filter.to_lowercase();
        names
            .iter()
            .enumerate()
            .filter(|(_, name)| {
                name.app_id.to_lowercase().contains(&lowered)
                    || name.title.to_lowercase().contains(&lowered)
            })
            .map(|(index, _)| index)
            .collect()
    } else {
        exact
    };
    match matches.as_slice() {
        [only] => Ok(*only),
        [] => Err(VshotError::Recording(format!(
            "no window matches `{filter}`; the compositor lists{}",
            describe_names(names)
        ))),
        several => Err(VshotError::Recording(format!(
            "`{filter}` matches {} windows:{}",
            several.len(),
            describe_names(
                &several
                    .iter()
                    .map(|index| names[*index])
                    .collect::<Vec<_>>()
            )
        ))),
    }
}

/// The window a compositor's own window list pointed at, as an index into
/// `names`.  The two descriptions of a window — the compositor's list and the
/// foreign-toplevel list — agree on the app id and the title, and that is what
/// this matches on: both together, then the title alone, then the app id alone,
/// and only where the answer is unambiguous.
pub fn match_description(names: &[&Name], app_id: &str, title: &str) -> Result<usize> {
    let app_id = app_id.trim();
    let title = title.trim();
    let by_app_id = |name: &Name| !app_id.is_empty() && name.app_id == app_id;
    let by_title = |name: &Name| !title.is_empty() && name.title == title;
    let mut matches: Vec<usize> = names
        .iter()
        .enumerate()
        .filter(|(_, name)| by_app_id(name) && by_title(name))
        .map(|(index, _)| index)
        .collect();
    if matches.is_empty() {
        matches = names
            .iter()
            .enumerate()
            .filter(|(_, name)| by_title(name))
            .map(|(index, _)| index)
            .collect();
    }
    if matches.is_empty() {
        matches = names
            .iter()
            .enumerate()
            .filter(|(_, name)| by_app_id(name))
            .map(|(index, _)| index)
            .collect();
    }
    match matches.as_slice() {
        [only] => Ok(*only),
        [] => Err(VshotError::Recording(format!(
            "the window the compositor points at (`{app_id} — {title}`) is not in the \
             foreign-toplevel list, which means this compositor describes its windows \
             differently; it lists{}",
            describe_names(names)
        ))),
        several => Err(VshotError::Recording(format!(
            "`{app_id} — {title}` matches {} windows:{}",
            several.len(),
            describe_names(
                &several
                    .iter()
                    .map(|index| names[*index])
                    .collect::<Vec<_>>()
            )
        ))),
    }
}

/// The windows as ` \n    label` lines, for an error that has to name what it
/// saw.  A listing of one window still gets the break, so the sentence around
/// it stays readable.
fn describe_names(names: &[&Name]) -> String {
    if names.is_empty() {
        return " no windows at all".to_owned();
    }
    names
        .iter()
        .map(|name| format!("\n    {}", name.describe()))
        .collect()
}

impl Dispatch<wl_registry::WlRegistry, ()> for CopyState {
    fn event(
        state: &mut Self,
        registry: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_registry::Event::Global {
            name,
            interface,
            version,
        } = event
        {
            match interface.as_str() {
                "ext_foreign_toplevel_list_v1" if state.list.is_none() => {
                    state.list = Some(registry.bind(name, version.min(1), qh, ()));
                }
                "ext_foreign_toplevel_image_capture_source_manager_v1"
                    if state.source_manager.is_none() =>
                {
                    state.source_manager = Some(registry.bind(name, version.min(1), qh, ()));
                }
                "ext_image_copy_capture_manager_v1" if state.manager.is_none() => {
                    state.manager = Some(registry.bind(name, version.min(1), qh, ()));
                }
                "zwp_linux_dmabuf_v1" if state.dmabuf.is_none() => {
                    // Version 3 is enough to create a buffer from a dma-buf, and
                    // binding lower keeps the compositor from sending the v4
                    // feedback events this client has no use for.
                    state.dmabuf = Some(registry.bind(name, version.min(3), qh, ()));
                }
                _ => {}
            }
        }
    }
}

impl Dispatch<ext_foreign_toplevel_list_v1::ExtForeignToplevelListV1, ()> for CopyState {
    fn event(
        state: &mut Self,
        _: &ext_foreign_toplevel_list_v1::ExtForeignToplevelListV1,
        event: ext_foreign_toplevel_list_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let ext_foreign_toplevel_list_v1::Event::Toplevel { toplevel } = event {
            state.listed.push(Listed {
                handle: toplevel,
                name: Name::default(),
                done: false,
                closed: false,
            });
        }
    }

    // `toplevel` is a `new_id`: wayland-client has to be told which object to
    // create for the event, and with which user data.
    wayland_client::event_created_child!(CopyState, ext_foreign_toplevel_list_v1::ExtForeignToplevelListV1, [
        ext_foreign_toplevel_list_v1::EVT_TOPLEVEL_OPCODE => (ext_foreign_toplevel_handle_v1::ExtForeignToplevelHandleV1, ())
    ]);
}

impl Dispatch<ext_foreign_toplevel_handle_v1::ExtForeignToplevelHandleV1, ()> for CopyState {
    fn event(
        state: &mut Self,
        handle: &ext_foreign_toplevel_handle_v1::ExtForeignToplevelHandleV1,
        event: ext_foreign_toplevel_handle_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        let Some(listed) = state
            .listed
            .iter_mut()
            .find(|listed| &listed.handle == handle)
        else {
            return;
        };
        match event {
            ext_foreign_toplevel_handle_v1::Event::AppId { app_id } => listed.name.app_id = app_id,
            ext_foreign_toplevel_handle_v1::Event::Title { title } => listed.name.title = title,
            ext_foreign_toplevel_handle_v1::Event::Identifier { identifier } => {
                listed.name.identifier = identifier;
            }
            ext_foreign_toplevel_handle_v1::Event::Done => listed.done = true,
            ext_foreign_toplevel_handle_v1::Event::Closed => listed.closed = true,
            _ => {}
        }
    }
}

impl Dispatch<ext_image_copy_capture_session_v1::ExtImageCopyCaptureSessionV1, ()> for CopyState {
    fn event(
        state: &mut Self,
        _: &ext_image_copy_capture_session_v1::ExtImageCopyCaptureSessionV1,
        event: ext_image_copy_capture_session_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        let Some(live) = state.live.as_mut() else {
            return;
        };
        match event {
            ext_image_copy_capture_session_v1::Event::BufferSize { width, height } => {
                live.buffer_size = Some((width, height));
            }
            ext_image_copy_capture_session_v1::Event::DmabufFormat { format, modifiers } => {
                live.formats
                    .push((format, modifiers_from_bytes(&modifiers)));
            }
            ext_image_copy_capture_session_v1::Event::Done => live.constraints_done = true,
            ext_image_copy_capture_session_v1::Event::Stopped => {
                live.stopped = true;
                live.pending = None;
            }
            ext_image_copy_capture_session_v1::Event::DmabufDevice { device } => {
                live.dmabuf_device = device;
            }
            // An shm offer is of no use to the zero-copy path.
            ext_image_copy_capture_session_v1::Event::ShmFormat { .. } => {}
            _ => {}
        }
    }
}

impl Dispatch<ext_image_copy_capture_frame_v1::ExtImageCopyCaptureFrameV1, ()> for CopyState {
    fn event(
        state: &mut Self,
        frame: &ext_image_copy_capture_frame_v1::ExtImageCopyCaptureFrameV1,
        event: ext_image_copy_capture_frame_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        let Some(pending) = state.live.as_mut().and_then(|live| live.pending.as_mut()) else {
            return;
        };
        if &pending.frame != frame {
            return;
        }
        match event {
            ext_image_copy_capture_frame_v1::Event::Ready => pending.ready = true,
            ext_image_copy_capture_frame_v1::Event::Failed { reason } => {
                pending.failed = Some(failure_code(reason));
            }
            // Transform, damage and the presentation time describe the frame
            // that is on its way; a recording timestamps frames from its own
            // clock and never flips them, so none of the three is acted on.
            ext_image_copy_capture_frame_v1::Event::Transform { .. }
            | ext_image_copy_capture_frame_v1::Event::Damage { .. }
            | ext_image_copy_capture_frame_v1::Event::PresentationTime { .. } => {}
            _ => {}
        }
    }
}

/// The numeric failure code, whether or not this build knows the name.  (A
/// code the protocol description this build carries does not name comes back as
/// [`FAILED_UNKNOWN`], which is exactly what "unknown" means here.)
fn failure_code(reason: WEnum<ext_image_copy_capture_frame_v1::FailureReason>) -> u32 {
    match reason {
        WEnum::Value(ext_image_copy_capture_frame_v1::FailureReason::Unknown) => FAILED_UNKNOWN,
        WEnum::Value(ext_image_copy_capture_frame_v1::FailureReason::BufferConstraints) => {
            FAILED_BUFFER_CONSTRAINTS
        }
        WEnum::Value(ext_image_copy_capture_frame_v1::FailureReason::Stopped) => FAILED_STOPPED,
        WEnum::Value(_) => FAILED_UNKNOWN,
        WEnum::Unknown(value) => value,
    }
}

/// The interfaces below send nothing a window capture acts on, but a client
/// that binds or creates them needs a `Dispatch` impl to receive anything at
/// all.
macro_rules! no_events {
    ($interface:ty) => {
        impl Dispatch<$interface, ()> for CopyState {
            fn event(
                _: &mut Self,
                _: &$interface,
                _: <$interface as wayland_client::Proxy>::Event,
                _: &(),
                _: &Connection,
                _: &QueueHandle<Self>,
            ) {
            }
        }
    };
}

no_events!(zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1);
no_events!(wl_buffer::WlBuffer);
no_events!(ext_image_capture_source_v1::ExtImageCaptureSourceV1);

/// The buffer params object sends one event, `failed`, and a window capture
/// cares about it: it is the only word a compositor gets to say that it could
/// not import the dma-buf it was handed (see [`WindowCapture::start`]).
impl Dispatch<zwp_linux_buffer_params_v1::ZwpLinuxBufferParamsV1, ()> for CopyState {
    fn event(
        state: &mut Self,
        _: &zwp_linux_buffer_params_v1::ZwpLinuxBufferParamsV1,
        event: zwp_linux_buffer_params_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let zwp_linux_buffer_params_v1::Event::Failed = event {
            if let Some(live) = state.live.as_mut() {
                live.params_failed = true;
            }
        }
    }
}
no_events!(
    ext_foreign_toplevel_image_capture_source_manager_v1::ExtForeignToplevelImageCaptureSourceManagerV1
);
no_events!(ext_image_copy_capture_manager_v1::ExtImageCopyCaptureManagerV1);

#[cfg(test)]
mod tests {
    use super::*;

    fn window(app_id: &str, title: &str) -> Name {
        Name {
            app_id: app_id.to_owned(),
            title: title.to_owned(),
            identifier: format!("id-{app_id}-{title}"),
        }
    }

    #[test]
    fn a_filter_takes_the_exact_name_before_a_substring() {
        let firefox = window("firefox", "Mozilla Firefox");
        let vivaldi = window("vivaldi-stable", "firefox documentation");
        let terminal = window("kitty", "~/dev/vshot");
        let names = [&firefox, &vivaldi, &terminal];
        // The exact app id wins even though the other window's title contains
        // the same word.
        assert_eq!(select(&names, "firefox").unwrap(), 0);
        // A substring of either still finds its window.
        assert_eq!(select(&names, "Mozilla").unwrap(), 0);
        assert_eq!(select(&names, "vivaldi").unwrap(), 1);
        assert_eq!(select(&names, "dev/vshot").unwrap(), 2);
        assert_eq!(select(&names, "KITTY").unwrap(), 2);
    }

    #[test]
    fn an_ambiguous_or_missing_filter_says_what_it_saw() {
        let one = window("kitty", "~/dev/vshot");
        let two = window("kitty", "~/dev/vshot/src");
        let names = [&one, &two];
        let error = select(&names, "kitty").unwrap_err().to_string();
        assert!(error.contains("matches 2 windows"), "{error}");
        // The listing carries both windows, so the user can tell them apart.
        assert!(error.contains("~/dev/vshot/src"), "{error}");
        let error = select(&names, "emacs").unwrap_err().to_string();
        assert!(error.contains("no window matches"), "{error}");
        assert!(error.contains("kitty"), "{error}");
        // Nothing to go on at all.
        assert!(select(&[], "kitty")
            .unwrap_err()
            .to_string()
            .contains("no windows at all"));
        assert!(select(&names, "  ")
            .unwrap_err()
            .to_string()
            .contains("empty"));
    }

    #[test]
    fn a_window_list_entry_is_matched_on_both_labels_then_either() {
        // Same title, different app ids: the pair decides.
        let first = window("kitty", "~/dev");
        let second = window("alacritty", "~/dev");
        let names = [&first, &second];
        assert_eq!(match_description(&names, "alacritty", "~/dev").unwrap(), 1);
        // A title the compositor reports differently still finds the window
        // through the app id.
        assert_eq!(match_description(&names, "kitty", "").unwrap(), 0);
        // And a title alone finds the only window that has it.
        let only = window("kitty", "~/dev/vshot");
        let names = [&only, &second];
        assert_eq!(match_description(&names, "", "~/dev/vshot").unwrap(), 0);
        // Two windows the same on both labels is not something to guess at.
        let twin = window("kitty", "~/dev");
        let names = [&first, &twin];
        let error = match_description(&names, "kitty", "~/dev")
            .unwrap_err()
            .to_string();
        assert!(error.contains("matches 2 windows"), "{error}");
        // A window the compositor's list knows and the toplevel list does not:
        // neither label of the entry appears in the list at all.
        let names = [&second];
        let error = match_description(&names, "kitty", "")
            .unwrap_err()
            .to_string();
        assert!(
            error.contains("describes its windows differently"),
            "{error}"
        );
    }

    #[test]
    fn the_format_is_one_the_encoder_can_take() {
        // The compositor's own order does not matter: XRGB8888 is preferred,
        // then ARGB8888, and anything else is not a candidate at all.
        let offered = vec![
            (0x3432_5241, vec![1 << 56, 0]),                         // ARGB8888
            (0x3033_4241, vec![0]),                                  // ABGR2101010, unusable here
            (0x3432_5258, vec![1 << 56, DRM_FORMAT_MOD_INVALID, 0]), // XRGB8888
        ];
        let (fourcc, modifiers) = pick_format(&offered).unwrap();
        assert_eq!(fourcc, DRM_FORMAT_XRGB8888);
        // Linear is the whole list when it is on offer: GBM ignores the order
        // of what it is handed and allocates a tiled buffer anyway, and this
        // compositor's EGL cannot import its own tiled buffers (measured — see
        // `order_modifiers`).
        assert_eq!(modifiers, vec![0]);
        // Only ARGB8888 on offer: taken, because the encoder takes it too.
        assert_eq!(
            pick_format(&[(DRM_FORMAT_ARGB8888, vec![0])]).unwrap().0,
            DRM_FORMAT_ARGB8888
        );
        // A format list that only carries unusable formats is a refusal, not a
        // guess.
        assert!(pick_format(&[(0x3033_4241, vec![0])]).is_none());
        assert!(pick_format(&[]).is_none());
        // A format without a single usable modifier cannot be allocated from.
        assert!(pick_format(&[(DRM_FORMAT_XRGB8888, Vec::new())]).is_none());
        assert!(
            pick_format(&[(DRM_FORMAT_XRGB8888, vec![DRM_FORMAT_MOD_INVALID])]).is_none(),
            "a buffer cannot be built on the invalid modifier"
        );
        // Without linear on offer the compositor's own list is what remains,
        // minus the invalid entry.
        let (_, modifiers) =
            pick_format(&[(DRM_FORMAT_XRGB8888, vec![1 << 56, DRM_FORMAT_MOD_INVALID])]).unwrap();
        assert_eq!(modifiers, vec![1 << 56]);
    }

    #[test]
    fn the_modifier_array_is_read_as_64_bit_values() {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&0u64.to_ne_bytes());
        bytes.extend_from_slice(&(1u64 << 56).to_ne_bytes());
        assert_eq!(modifiers_from_bytes(&bytes), vec![0, 1 << 56]);
        // A truncated array (a compositor bug) drops the partial value rather
        // than reading past it.
        assert_eq!(modifiers_from_bytes(&bytes[..12]), vec![0]);
        assert!(modifiers_from_bytes(&[]).is_empty());
    }

    /// Exercises the real protocols against a live session: it lists the
    /// windows, opens a session on the first one and takes a few frames out of
    /// it.  Ignored by default because it needs a compositor that offers window
    /// capture and at least one mapped window; run it from inside the session
    /// with `cargo test -- --ignored captures_a_live_window --nocapture`.
    #[test]
    #[ignore = "needs a live session with a window on it"]
    fn captures_a_live_window() {
        let mut capture = WindowCapture::connect().expect("this session offers window capture");
        let toplevels = capture
            .toplevels()
            .expect("the compositor lists its windows");
        eprintln!("vshot: {} window(s) listed", toplevels.len());
        for toplevel in &toplevels {
            eprintln!(
                "vshot:   app_id `{}` title `{}` identifier `{}`",
                toplevel.name.app_id, toplevel.name.title, toplevel.name.identifier
            );
        }
        // The window under test can be named with VSHOT_WINDOW_PROBE, which is
        // how a window on the current workspace gets picked when the first
        // entry of the list sits on another one.
        let filter = std::env::var("VSHOT_WINDOW_PROBE").ok();
        let index = match filter.as_deref() {
            Some(filter) => {
                let names: Vec<&Name> = toplevels.iter().map(|toplevel| &toplevel.name).collect();
                select(&names, filter).expect("the filter names one window")
            }
            None => 0,
        };
        let toplevel = &toplevels[index];
        eprintln!("vshot: recording `{}`", toplevel.label());
        let shape = capture.start(toplevel).expect("the session starts");
        eprintln!(
            "vshot: session 0x{:08x} {}x{}",
            shape.fourcc, shape.width, shape.height
        );
        let interrupted = AtomicBool::new(false);
        let deadline = Instant::now() + Duration::from_secs(5);
        let mut frames = 0;
        while Instant::now() < deadline && frames < 3 {
            match capture
                .grab(Duration::from_millis(500), &interrupted)
                .expect("a grab")
            {
                Capture::Frame(frame) => {
                    frames += 1;
                    eprintln!(
                        "vshot: frame {}x{} stride {} modifier 0x{:x}",
                        frame.width, frame.height, frame.stride, frame.modifier
                    );
                }
                Capture::Idle => eprintln!("vshot: nothing ready yet"),
                Capture::Ended(reason) => {
                    eprintln!("vshot: the session ended: {reason}");
                    break;
                }
                Capture::Interrupted => break,
            }
        }
        assert!(frames > 0, "at least one frame has to come back");
    }
}
