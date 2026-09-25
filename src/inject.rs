// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Driving the wheel of whatever page is under the pointer.
//!
//! A scrolling capture has to scroll the page itself, and Wayland has no
//! protocol for "scroll this window": the wheel has to look like input.  Two
//! backends do that, picked at runtime:
//!
//! * `zwlr_virtual_pointer_manager_v1` — the compositor hands out a virtual
//!   pointer and the wheel travels through a public Wayland protocol.  This is
//!   what Hyprland, sway and niri (wlroots and smithay) provide, needs no
//!   device permissions and no helper program.  Preferred whenever available.
//! * `/dev/uinput` — a kernel-level virtual mouse, which works on any
//!   compositor but needs write access to the device node.  This is the
//!   fallback for compositors that do not speak the wlr protocol, KWin among
//!   them.
//!
//! Both are used through [`Injector`], which hides the choice from the
//! scrolling loop.

use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::Write;
use std::os::fd::AsRawFd;
use std::os::raw::{c_int, c_ulong};
use std::time::Duration;

use zbus::blocking::{Connection as BusConnection, MessageIterator, Proxy};
use zbus::zvariant::{OwnedObjectPath, OwnedValue, Value};

use wayland_client::protocol::{wl_pointer, wl_registry, wl_seat};
use wayland_client::{Connection, Dispatch, EventQueue, QueueHandle};
use wayland_protocols_wlr::virtual_pointer::v1::client::{
    zwlr_virtual_pointer_manager_v1, zwlr_virtual_pointer_v1,
};

use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect};

/// Device node of the kernel's input injection interface.
const UINPUT_PATH: &str = "/dev/uinput";

/// How long to let the compositor notice a freshly created virtual uinput
/// device before the first event lands: udev has to see it and libinput has to
/// open it, which is tens of milliseconds on an idle machine.
const DEVICE_SETTLE: Duration = Duration::from_millis(250);

/// Which backend the user asked for.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Prefer {
    /// Try the compositor protocol, then the portal, then `/dev/uinput`.
    Auto,
    /// Only `zwlr_virtual_pointer_manager_v1`.
    Wlr,
    /// Only the XDG RemoteDesktop portal.
    Portal,
    /// Only `/dev/uinput`.
    Uinput,
}

impl Prefer {
    /// Parses the `--inject` value.
    pub fn parse(value: &str) -> Result<Self> {
        match value {
            "auto" => Ok(Self::Auto),
            "wlr" => Ok(Self::Wlr),
            "portal" => Ok(Self::Portal),
            "uinput" => Ok(Self::Uinput),
            other => Err(VshotError::InvalidDestination(format!(
                "`--inject {other}` is not one of auto, wlr, portal, uinput"
            ))),
        }
    }
}

/// Scrolls the page under the pointer through whichever backend is available.
pub struct Injector {
    backend: Backend,
}

enum Backend {
    Wlr(Box<WlrPointer>),
    Portal(PortalRemote),
    Uinput(UinputWheel),
}

impl Injector {
    /// Opens an injector.  `desktop` is the logical bounding box of every
    /// output, which is the coordinate space the protocol backend expresses
    /// absolute pointer positions in.
    pub fn open(desktop: Rect, prefer: Prefer) -> Result<Self> {
        match prefer {
            Prefer::Wlr => Ok(Self {
                backend: Backend::Wlr(Box::new(WlrPointer::open(desktop)?)),
            }),
            Prefer::Portal => Ok(Self {
                backend: Backend::Portal(PortalRemote::open()?),
            }),
            Prefer::Uinput => Ok(Self {
                backend: Backend::Uinput(UinputWheel::open()?),
            }),
            // From the least intrusive to the most: the compositor's own
            // protocol needs nothing configured and nothing granted, the
            // portal needs one permission dialog, and `/dev/uinput` needs
            // access to a device node.
            Prefer::Auto => {
                let mut reasons = Vec::new();
                match WlrPointer::open(desktop) {
                    Ok(pointer) => {
                        return Ok(Self {
                            backend: Backend::Wlr(Box::new(pointer)),
                        })
                    }
                    Err(error) => reasons.push(format!("no virtual pointer ({error})")),
                }
                match PortalRemote::open() {
                    Ok(portal) => {
                        return Ok(Self {
                            backend: Backend::Portal(portal),
                        })
                    }
                    Err(error) => reasons.push(format!("no remote-desktop portal ({error})")),
                }
                match UinputWheel::open() {
                    Ok(wheel) => {
                        return Ok(Self {
                            backend: Backend::Uinput(wheel),
                        })
                    }
                    Err(error) => reasons.push(format!("{UINPUT_PATH} is unusable ({error})")),
                }
                Err(VshotError::LongShotInjection(format!(
                    "this session offers no way to scroll: {}",
                    reasons.join("; ")
                )))
            }
        }
    }

    /// Which backend ended up being used, for logs and error messages.
    pub fn backend_name(&self) -> &'static str {
        match self.backend {
            Backend::Wlr(_) => "wlr-virtual-pointer",
            Backend::Portal(_) => "remote-desktop portal",
            Backend::Uinput(_) => "/dev/uinput",
        }
    }

    /// One wheel click is one notch; positive scrolls down.
    pub fn scroll(&mut self, clicks: i32) -> Result<()> {
        match &mut self.backend {
            Backend::Wlr(pointer) => pointer.scroll(clicks),
            Backend::Portal(portal) => portal.scroll(clicks),
            Backend::Uinput(wheel) => wheel.scroll(clicks),
        }
    }

    /// Moves the pointer to a global logical position.  Backends that cannot
    /// move it do nothing: the pointer has just been used to pick the region,
    /// so it is already where it needs to be.  (The portal could move it, but
    /// only relatively — and where the pointer currently is is not something a
    /// Wayland client gets to ask.)
    pub fn move_pointer(&mut self, point: Point) -> Result<()> {
        match &mut self.backend {
            Backend::Wlr(pointer) => pointer.move_pointer(point),
            Backend::Portal(_) | Backend::Uinput(_) => Ok(()),
        }
    }
}

// -- the wlr virtual pointer ------------------------------------------------

#[derive(Default)]
struct WlrState {
    manager: Option<zwlr_virtual_pointer_manager_v1::ZwlrVirtualPointerManagerV1>,
    seat: Option<wl_seat::WlSeat>,
}

impl Dispatch<wl_registry::WlRegistry, ()> for WlrState {
    fn event(
        state: &mut Self,
        registry: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _data: &(),
        _connection: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        let wl_registry::Event::Global {
            name,
            interface,
            version,
        } = event
        else {
            return;
        };
        match interface.as_str() {
            "zwlr_virtual_pointer_manager_v1" => {
                state.manager = Some(registry.bind(name, version.min(2), qh, ()));
            }
            "wl_seat" => {
                state.seat = Some(registry.bind(name, version.min(7), qh, ()));
            }
            _ => {}
        }
    }
}

impl Dispatch<wl_seat::WlSeat, ()> for WlrState {
    fn event(
        _state: &mut Self,
        _seat: &wl_seat::WlSeat,
        _event: wl_seat::Event,
        _data: &(),
        _connection: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        // Capabilities are irrelevant: the virtual pointer is created against
        // the seat object, not against what the seat currently has.
    }
}

impl Dispatch<zwlr_virtual_pointer_manager_v1::ZwlrVirtualPointerManagerV1, ()> for WlrState {
    fn event(
        _state: &mut Self,
        _manager: &zwlr_virtual_pointer_manager_v1::ZwlrVirtualPointerManagerV1,
        _event: zwlr_virtual_pointer_manager_v1::Event,
        _data: &(),
        _connection: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        unreachable!("zwlr_virtual_pointer_manager_v1 has no events")
    }
}

impl Dispatch<zwlr_virtual_pointer_v1::ZwlrVirtualPointerV1, ()> for WlrState {
    fn event(
        _state: &mut Self,
        _pointer: &zwlr_virtual_pointer_v1::ZwlrVirtualPointerV1,
        _event: zwlr_virtual_pointer_v1::Event,
        _data: &(),
        _connection: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        unreachable!("zwlr_virtual_pointer_v1 has no events")
    }
}

/// Wheel injection through `zwlr_virtual_pointer_manager_v1`.
struct WlrPointer {
    connection: Connection,
    queue: EventQueue<WlrState>,
    state: WlrState,
    pointer: zwlr_virtual_pointer_v1::ZwlrVirtualPointerV1,
    /// Logical bounds of the whole desktop; absolute positions are relative to
    /// its origin and scaled to its size.
    desktop: Rect,
}

impl WlrPointer {
    fn open(desktop: Rect) -> Result<Self> {
        let connection = Connection::connect_to_env()
            .map_err(|error| VshotError::WaylandConnection(error.to_string()))?;
        let mut queue: EventQueue<WlrState> = connection.new_event_queue();
        let qh = queue.handle();
        let mut state = WlrState::default();
        connection.display().get_registry(&qh, ());
        for _ in 0..2 {
            queue
                .roundtrip(&mut state)
                .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        }
        let manager = state.manager.take().ok_or_else(|| {
            VshotError::LongShotInjection(
                "the compositor does not expose zwlr_virtual_pointer_manager_v1".into(),
            )
        })?;
        let seat = state.seat.take().ok_or_else(|| {
            VshotError::LongShotInjection("the compositor exposes no wl_seat".into())
        })?;
        let pointer = manager.create_virtual_pointer(Some(&seat), &qh, ());
        queue
            .roundtrip(&mut state)
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        Ok(Self {
            connection,
            queue,
            state,
            pointer,
            desktop,
        })
    }

    fn commit(&mut self) -> Result<()> {
        self.connection
            .flush()
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        // A roundtrip makes sure the compositor has actually processed the
        // events before the caller starts waiting for the page to move.
        self.queue
            .roundtrip(&mut self.state)
            .map(|_| ())
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))
    }

    fn scroll(&mut self, clicks: i32) -> Result<()> {
        // A discrete step is worth 10 units on the wire, which is what a real
        // pointer sends and what toolkits expect to see.
        self.pointer.axis_discrete(
            0,
            wl_pointer::Axis::VerticalScroll,
            f64::from(clicks) * 10.0,
            clicks,
        );
        self.pointer.frame();
        self.commit()
    }

    fn move_pointer(&mut self, point: Point) -> Result<()> {
        // The coordinates are absolute in the desktop layout, in logical
        // pixels, and the extents that go with them are the layout's — that is
        // what a wlroots-style compositor does with this request (measured on
        // Hyprland: with the pointer parked on one output, asking for a point
        // on another lands on exactly that point, not on a fraction of the
        // output the pointer started on).  Which is why the backend needs the
        // desktop bounds.
        let width = self.desktop.size.width;
        let height = self.desktop.size.height;
        let x = (point.x - self.desktop.origin.x).clamp(0, width as i32) as u32;
        let y = (point.y - self.desktop.origin.y).clamp(0, height as i32) as u32;
        self.pointer.motion_absolute(0, x, y, width, height);
        self.pointer.frame();
        self.commit()
    }
}

impl Drop for WlrPointer {
    fn drop(&mut self) {
        self.pointer.destroy();
        if let Some(manager) = self.state.manager.take() {
            manager.destroy();
        }
        // Best effort: the connection is going away right after this anyway,
        // and the compositor drops the virtual pointer with it.
        let _ = self.connection.flush();
    }
}

// -- /dev/uinput ------------------------------------------------------------

// From `linux/uinput.h`: _IOW('U', 100, int), _IOW('U', 102, int),
// _IOW('U', 3, struct uinput_setup), _IO('U', 1), _IO('U', 2).  Both the ioctl
// number *and* the encoded size matter — the kernel switches on the number, so
// a request built with the wrong size is handled as a different command
// entirely (which is how `UI_DEV_SETUP` first came out as `UI_SET_EVBIT`).
const UI_SET_EVBIT: c_ulong = 0x4004_5564;
const UI_SET_RELBIT: c_ulong = 0x4004_5566;
const UI_DEV_SETUP: c_ulong = 0x405c_5503;
const UI_DEV_CREATE: c_ulong = 0x5501;
const UI_DEV_DESTROY: c_ulong = 0x5502;

const EV_SYN: u16 = 0x00;
const EV_REL: u16 = 0x02;
const SYN_REPORT: u16 = 0x00;
const REL_HWHEEL: u16 = 0x06;
const REL_WHEEL: u16 = 0x08;

extern "C" {
    fn ioctl(fd: c_int, request: c_ulong, ...) -> c_int;
}

/// `struct input_event` with a 64-bit `timeval` (two 8-byte fields), which is
/// how it is laid out on every 64-bit Linux target.
#[repr(C)]
struct InputEvent {
    tv_sec: i64,
    tv_usec: i64,
    kind: u16,
    code: u16,
    value: i32,
}

/// `UINPUT_MAX_NAME_SIZE` from `linux/uinput.h`.
const UINPUT_MAX_NAME_SIZE: usize = 80;

/// The identity the wheel registers itself under.  `BUS_USB` is what `ydotool`
/// uses for its virtual mouse, and a plain mouse is what libinput expects to see
/// behind a device that only offers a wheel.
const UINPUT_BUS_USB: u16 = 0x03;
const UINPUT_NAME: &[u8] = b"vshot virtual wheel";

/// `struct uinput_setup` from `linux/uinput.h`: a `struct input_id` (four
/// `__u16`s), the fixed-size name, and the force-feedback effect count.
///
/// The kernel refuses `UI_DEV_CREATE` with `EINVAL` for a device that was never
/// set up — it would be registered without a name — so this has to be handed
/// over first.  The size of this struct is encoded in `UI_DEV_SETUP`, which is
/// why a test pins it.
#[repr(C)]
struct UinputSetup {
    bustype: u16,
    vendor: u16,
    product: u16,
    version: u16,
    name: [u8; UINPUT_MAX_NAME_SIZE],
    ff_effects_max: u32,
}

/// Wheel injection through a kernel virtual mouse.
struct UinputWheel {
    device: File,
}

impl UinputWheel {
    fn open() -> Result<Self> {
        let device = OpenOptions::new()
            .write(true)
            .open(UINPUT_PATH)
            .map_err(|error| {
                VshotError::LongShotInjection(format!(
                    "cannot open {UINPUT_PATH} ({error}); add this user to the `input` group, \
                     or install a udev rule giving the device TAG+=\"uaccess\""
                ))
            })?;
        let fd = device.as_raw_fd();
        for (request, capability) in [
            (UI_SET_EVBIT, EV_REL),
            (UI_SET_RELBIT, REL_WHEEL),
            (UI_SET_RELBIT, REL_HWHEEL),
        ] {
            if unsafe { ioctl(fd, request, c_ulong::from(capability)) } < 0 {
                return Err(VshotError::LongShotInjection(format!(
                    "{UINPUT_PATH} refused the wheel capability {capability} ({})",
                    std::io::Error::last_os_error()
                )));
            }
        }
        let mut setup = UinputSetup {
            bustype: UINPUT_BUS_USB,
            vendor: 0x1234,
            product: 0x5678,
            version: 1,
            name: [0; UINPUT_MAX_NAME_SIZE],
            ff_effects_max: 0,
        };
        setup.name[..UINPUT_NAME.len()].copy_from_slice(UINPUT_NAME);
        if unsafe { ioctl(fd, UI_DEV_SETUP, std::ptr::from_ref(&setup)) } < 0 {
            return Err(VshotError::LongShotInjection(format!(
                "{UINPUT_PATH} refused to set the device up ({})",
                std::io::Error::last_os_error()
            )));
        }
        if unsafe { ioctl(fd, UI_DEV_CREATE) } < 0 {
            return Err(VshotError::LongShotInjection(format!(
                "{UINPUT_PATH} refused to create the device ({})",
                std::io::Error::last_os_error()
            )));
        }
        std::thread::sleep(DEVICE_SETTLE);
        Ok(Self { device })
    }

    /// The two events one notch travels as: the relative wheel itself and the
    /// sync report that closes the packet — a relative event only takes effect
    /// once the report arrives.
    ///
    /// The sign flips here.  evdev counts `REL_WHEEL` *up* for a wheel turned
    /// away from the user, while this type's callers count up as "scroll down"
    /// (the way `wl_pointer.axis` and the portal both spell it).  Handing the
    /// count over unchanged is what made a capture walk the page the wrong way.
    fn events(clicks: i32) -> [InputEvent; 2] {
        [
            InputEvent {
                tv_sec: 0,
                tv_usec: 0,
                kind: EV_REL,
                code: REL_WHEEL,
                value: -clicks,
            },
            InputEvent {
                tv_sec: 0,
                tv_usec: 0,
                kind: EV_SYN,
                code: SYN_REPORT,
                value: 0,
            },
        ]
    }

    fn scroll(&mut self, clicks: i32) -> Result<()> {
        let events = Self::events(clicks);
        let bytes = unsafe {
            std::slice::from_raw_parts(events.as_ptr().cast::<u8>(), std::mem::size_of_val(&events))
        };
        self.device.write_all(bytes).map_err(|error| {
            VshotError::LongShotInjection(format!("writing to {UINPUT_PATH} failed: {error}"))
        })
    }
}

impl Drop for UinputWheel {
    fn drop(&mut self) {
        let _ = unsafe { ioctl(self.device.as_raw_fd(), UI_DEV_DESTROY) };
    }
}

// -- the XDG RemoteDesktop portal -------------------------------------------

const PORTAL_SERVICE: &str = "org.freedesktop.portal.Desktop";
const PORTAL_PATH: &str = "/org/freedesktop/portal/desktop";
const PORTAL_REMOTE_DESKTOP: &str = "org.freedesktop.portal.RemoteDesktop";
const PORTAL_REQUEST: &str = "org.freedesktop.portal.Request";
const PORTAL_SESSION: &str = "org.freedesktop.portal.Session";

/// `SelectDevices` bit for the pointer (only the wheel is ever used).
const PORTAL_DEVICE_POINTER: u32 = 2;
/// `NotifyPointerAxisDiscrete`: 0 is the vertical wheel.
const PORTAL_AXIS_VERTICAL: u32 = 0;

/// Wheel injection through the XDG RemoteDesktop portal.
///
/// KDE and GNOME implement this portal for their remote-desktop tools, which
/// makes it the way to drive the pointer on a compositor that neither speaks
/// `zwlr_virtual_pointer_manager_v1` nor hands `/dev/uinput` to the user:
/// everything travels over the session bus and the compositor asks permission
/// once.  On KWin the far side is `org.kde.KWin.EIS.RemoteDesktop`.
///
/// The calls that set the session up are two-phase by design: each returns a
/// request handle and answers with a `Response` signal on it, so the signal
/// stream is subscribed *before* the call — a fast reply would otherwise land
/// before anyone was listening.  `Start` is the one that puts the permission
/// dialog on screen, and it blocks until the user answers it.
struct PortalRemote {
    connection: BusConnection,
    session: OwnedObjectPath,
}

impl PortalRemote {
    fn open() -> Result<Self> {
        let connection = BusConnection::session().map_err(|error| {
            VshotError::LongShotInjection(format!("no session bus for the portal: {error}"))
        })?;
        let proxy = portal_proxy(&connection, PORTAL_PATH, PORTAL_REMOTE_DESKTOP)?;
        let mut responses = response_stream(&connection)?;

        let mut options: HashMap<&str, Value<'_>> = HashMap::new();
        options.insert("handle_token", Value::from(request_token("create")));
        options.insert(
            "session_handle_token",
            Value::from(request_token("session")),
        );
        // `CreateSession` takes the options dict and nothing else — both tokens
        // travel inside it.  A leading empty string (the shape
        // `CreateSession(s session_handle_token, a{sv} options)`) is answered
        // with `InvalidArgs` by xdg-desktop-portal 1.20.
        let request: OwnedObjectPath = call(&proxy, "CreateSession", &(options,))?;
        let results = wait_response(&mut responses, &request)?;
        let session = result_object(&results, "session_handle")?;

        let mut options: HashMap<&str, Value<'_>> = HashMap::new();
        options.insert("handle_token", Value::from(request_token("devices")));
        options.insert("types", Value::from(PORTAL_DEVICE_POINTER));
        let request: OwnedObjectPath = call(&proxy, "SelectDevices", &(session.clone(), options))?;
        wait_response(&mut responses, &request)?;

        let mut options: HashMap<&str, Value<'_>> = HashMap::new();
        options.insert("handle_token", Value::from(request_token("start")));
        let request: OwnedObjectPath = call(&proxy, "Start", &(session.clone(), "", options))?;
        wait_response(&mut responses, &request)?;

        Ok(Self {
            connection,
            session,
        })
    }

    fn scroll(&mut self, clicks: i32) -> Result<()> {
        let proxy = portal_proxy(&self.connection, PORTAL_PATH, PORTAL_REMOTE_DESKTOP)?;
        let options: HashMap<&str, Value<'_>> = HashMap::new();
        call::<()>(
            &proxy,
            "NotifyPointerAxisDiscrete",
            &(self.session.clone(), options, PORTAL_AXIS_VERTICAL, clicks),
        )
    }
}

impl Drop for PortalRemote {
    fn drop(&mut self) {
        // Closing the session gives the permission back: the portal keeps it
        // for exactly as long as the session lives.
        if let Ok(proxy) = portal_proxy(&self.connection, self.session.as_str(), PORTAL_SESSION) {
            let _ = call::<()>(&proxy, "Close", &());
        }
    }
}

/// An owned proxy: it holds its own handle on the bus, so it does not borrow
/// the connection that outlives it inside the session struct.
fn portal_proxy(connection: &BusConnection, path: &str, interface: &str) -> Result<Proxy<'static>> {
    Proxy::new_owned(
        connection.clone(),
        PORTAL_SERVICE,
        path.to_string(),
        interface.to_string(),
    )
    .map_err(|error| {
        VshotError::LongShotInjection(format!("the portal does not offer `{interface}`: {error}"))
    })
}

fn call<R>(
    proxy: &Proxy<'_>,
    method: &str,
    body: &(impl serde::Serialize + zbus::zvariant::DynamicType),
) -> Result<R>
where
    R: serde::de::DeserializeOwned + zbus::zvariant::Type,
{
    proxy.call(method, body).map_err(|error| {
        VshotError::LongShotInjection(format!("portal call `{method}` failed: {error}"))
    })
}

/// Watches every `org.freedesktop.portal.Request.Response` on the bus; the
/// caller picks the one belonging to its own request handle.
fn response_stream(connection: &BusConnection) -> Result<MessageIterator> {
    let rule = zbus::MatchRule::builder()
        .msg_type(zbus::message::Type::Signal)
        .interface(PORTAL_REQUEST)
        .map_err(|error| {
            VshotError::LongShotInjection(format!("cannot build the portal match rule: {error}"))
        })?
        .member("Response")
        .map_err(|error| {
            VshotError::LongShotInjection(format!("cannot build the portal match rule: {error}"))
        })?
        .build();
    MessageIterator::for_match_rule(rule, connection, None).map_err(|error| {
        VshotError::LongShotInjection(format!("cannot watch portal responses: {error}"))
    })
}

/// Blocks until the answer to `request` arrives.  A caller that gives up on
/// the permission dialog leaves this waiting, which is what Ctrl+C is for.
fn wait_response(
    responses: &mut MessageIterator,
    request: &OwnedObjectPath,
) -> Result<HashMap<String, OwnedValue>> {
    for message in responses.by_ref() {
        let message = message.map_err(|error| {
            VshotError::LongShotInjection(format!("portal response failed: {error}"))
        })?;
        if message.header().path().map(|path| path.as_str()) != Some(request.as_str()) {
            continue;
        }
        let (code, results): (u32, HashMap<String, OwnedValue>) =
            message.body().deserialize().map_err(|error| {
                VshotError::LongShotInjection(format!("cannot read the portal's answer: {error}"))
            })?;
        if code != 0 {
            return Err(VshotError::LongShotInjection(match code {
                1 => "the remote-desktop request was cancelled".into(),
                2 => "the remote-desktop request was denied".into(),
                other => format!("the remote-desktop request failed with code {other}"),
            }));
        }
        return Ok(results);
    }
    Err(VshotError::LongShotInjection(
        "the portal closed its response stream".into(),
    ))
}

/// Reads an object path out of a portal response.  `session_handle` is specified
/// as an object path, but the interface XML admits it "was erroneously
/// implemented as `s`" and that this will stay for compatibility, so both
/// spellings are accepted.  Anything else is an error rather than a guess.
fn result_object(results: &HashMap<String, OwnedValue>, key: &str) -> Result<OwnedObjectPath> {
    let value = results.get(key).ok_or_else(|| {
        VshotError::LongShotInjection(format!("the portal did not report `{key}`"))
    })?;
    if let Ok(path) = OwnedObjectPath::try_from(value.clone()) {
        return Ok(path);
    }
    let text = value.downcast_ref::<&str>().map_err(|error| {
        VshotError::LongShotInjection(format!(
            "the portal's `{key}` is neither an object path nor a string: {error}"
        ))
    })?;
    OwnedObjectPath::try_from(text.to_string()).map_err(|error| {
        VshotError::LongShotInjection(format!(
            "the portal's `{key}` is a string that is not an object path: {error}"
        ))
    })
}

/// A token the portal echoes back in the request path it creates for us.  It
/// has to be unique-ish and to use `[A-Za-z0-9_]` only.
fn request_token(prefix: &str) -> String {
    format!("vshot{}_{prefix}", std::process::id())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_wheel_event_layout_is_what_the_kernel_expects() {
        // 24 bytes on a 64-bit target: two timespec-sized fields, then the
        // type/code/value triple.
        assert_eq!(std::mem::size_of::<InputEvent>(), 24);
        assert_eq!(std::mem::offset_of!(InputEvent, kind), 16);
        assert_eq!(std::mem::offset_of!(InputEvent, value), 20);
    }

    #[test]
    fn the_device_setup_matches_the_request_that_carries_it() {
        // `_IOW` encodes the argument size, and the kernel matches on the ioctl
        // number: a request whose size disagrees with the struct it points at
        // gets dispatched as whatever command shares that number.
        let encoded_size = ((UI_DEV_SETUP >> 16) & 0xff) as usize;
        assert_eq!(encoded_size, std::mem::size_of::<UinputSetup>());
        assert_eq!(std::mem::size_of::<UinputSetup>(), 92);
        assert_eq!(std::mem::offset_of!(UinputSetup, name), 8);
        assert_eq!(std::mem::offset_of!(UinputSetup, ff_effects_max), 88);
        assert!(UINPUT_NAME.len() < UINPUT_MAX_NAME_SIZE);
    }

    #[test]
    fn the_wheel_arrives_counting_the_way_the_callers_do() {
        // `WheelInjector::scroll` counts positive as "scroll down"; evdev's
        // vertical wheel counts *up* for a wheel turned away from the user, so
        // the sign has to flip on the way to the device.
        let down = UinputWheel::events(1);
        assert_eq!(
            (down[0].kind, down[0].code, down[0].value),
            (EV_REL, REL_WHEEL, -1)
        );
        let up = UinputWheel::events(-3);
        assert_eq!(up[0].value, 3);
        // A relative event is only acted on once the report closing the packet
        // follows it.
        for events in [down, up] {
            assert_eq!(
                (events[1].kind, events[1].code, events[1].value),
                (EV_SYN, SYN_REPORT, 0)
            );
        }
    }

    /// Creates a real virtual mouse, which is the only way to know the kernel
    /// accepts the device: `UI_DEV_CREATE` is where a missing `UI_DEV_SETUP`
    /// shows up as `EINVAL`.  Ignored by default because it needs `/dev/uinput`
    /// to be writable by the user running the tests.
    #[test]
    #[ignore = "needs write access to /dev/uinput"]
    fn a_wheel_only_device_can_be_created() {
        let mut wheel = UinputWheel::open().expect("/dev/uinput has to accept a wheel device");
        wheel
            .scroll(1)
            .expect("the created device has to take a wheel event");
    }

    #[test]
    fn the_session_handle_is_read_whatever_the_portal_calls_it() {
        // The RemoteDesktop interface XML says the handle "was erroneously
        // implemented as `s`" and will stay that way, so both spellings have to
        // be understood — and anything else has to be refused.
        let handle = "/org/freedesktop/portal/desktop/session/1_1/vshot1";
        let mut results: HashMap<String, OwnedValue> = HashMap::new();
        results.insert(
            "session_handle".into(),
            Value::from(handle).try_into().unwrap(),
        );
        assert_eq!(
            result_object(&results, "session_handle").unwrap().as_str(),
            handle
        );
        results.insert(
            "session_handle".into(),
            Value::from(zbus::zvariant::ObjectPath::try_from(handle).unwrap())
                .try_into()
                .unwrap(),
        );
        assert_eq!(
            result_object(&results, "session_handle").unwrap().as_str(),
            handle
        );
        results.insert(
            "session_handle".into(),
            Value::from(7u32).try_into().unwrap(),
        );
        assert!(result_object(&results, "session_handle").is_err());
        assert!(result_object(&results, "missing").is_err());
    }

    #[test]
    fn prefer_parses_its_four_spellings() {
        assert_eq!(Prefer::parse("auto").unwrap(), Prefer::Auto);
        assert_eq!(Prefer::parse("wlr").unwrap(), Prefer::Wlr);
        assert_eq!(Prefer::parse("portal").unwrap(), Prefer::Portal);
        assert_eq!(Prefer::parse("uinput").unwrap(), Prefer::Uinput);
        let error = Prefer::parse("mouse").unwrap_err();
        assert!(
            error.to_string().contains("auto, wlr, portal, uinput"),
            "{error}"
        );
    }
}
