pub mod window;
pub mod window_pixel;
pub mod wlr;

pub use window::{CompositorWindowProvider, ProcessWindowProvider};
pub use window_pixel::detect_active_window;
pub use wlr::WlrCapture;
